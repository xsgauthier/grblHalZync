#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

// Mandated PG080 Register Offsets (relative to the base address)
#define XLLF_ISR_OFFSET  0x00  // Interrupt Status Register
#define XLLF_IER_OFFSET  0x04  // Interrupt Enable Register
#define XLLF_TDFR_OFFSET 0x08  // Transmit Data FIFO Reset
#define XLLF_TDFV_OFFSET 0x0C  // Transmit Data FIFO Vacancy
#define XLLF_RDFR_OFFSET 0x18  // Receive Data FIFO Reset
#define XLLF_RDFO_OFFSET 0x1C  // Receive Data FIFO Occupancy
#define XLLF_RLR_OFFSET  0x24  // Receive Length Register

// Bitmasks for parsing the Interrupt Status Register (ISR)
#define XLLF_INT_RC_MASK   0x04000000  // Receive Complete
#define XLLF_INT_TC_MASK   0x08000000  // Transmit Complete
#define XLLF_INT_TPO_MASK  0x10000000  // Transmit Programmable Empty
#define XLLF_INT_RPO_MASK  0x20000000  // Receive Programmable Empty
#define XLLF_INT_RPURE_MASK 0x01000000 // Receive Under Read Error
#define XLLF_INT_RPORE_MASK 0x02000000 // Receive Over Read Error

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <AXI_FIFO_PHYSICAL_BASE_ADDRESS>\n", argv[0]);
        fprintf(stderr, "Example: %s 0x43C00000\n", argv[0]);
        return EXIT_FAILURE;
    }

    // Parse base address argument
    uintptr_t phys_addr = strtoul(argv[1], NULL, 0);
    long pagesize = sysconf(_SC_PAGESIZE);
    uintptr_t base_page = phys_addr & ~(pagesize - 1);
    uintptr_t page_offset = phys_addr - base_page;

    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) {
        perror("Error: Cannot open /dev/mem (Are you running with sudo?)");
        return EXIT_FAILURE;
    }

    // Map the hardware register space
    void *map_base = mmap(NULL, pagesize, PROT_READ, MAP_SHARED, fd, base_page);
    if (map_base == MAP_FAILED) {
        perror("Error: Memory map (mmap) failed");
        close(fd);
        return EXIT_FAILURE;
    }

    // Volatile pointer sequence to ensure no compiler optimization cache
    volatile uint32_t *fifo_reg = (volatile uint32_t *)((char *)map_base + page_offset);

    printf("==================================================\n");
    printf("  AXI-Stream FIFO (PG080) Diagnostics via devmem  \n");
    printf("  Target Physical Base Address: 0x%08lX\n", (unsigned long)phys_addr);
    printf("==================================================\n\n");

    // READ SEQUENCE: CRITICAL DESIGN CONSTRAINT (PG080 Documentation)
    // RDFO *MUST* be read before reading RLR. 
    // Reading RLR first will instantly reset RDFO to zero.
    uint32_t isr  = fifo_reg[XLLF_ISR_OFFSET / 4];
    uint32_t ier  = fifo_reg[XLLF_IER_OFFSET / 4];
    uint32_t tdfv = fifo_reg[XLLF_TDFV_OFFSET / 4];
    uint32_t rdfo = fifo_reg[XLLF_RDFO_OFFSET / 4]; 
    uint32_t rlr  = fifo_reg[XLLF_RLR_OFFSET / 4];  

    printf("[RAW REGISTER DUMP]\n");
    printf("  ISR  (0x00): 0x%08X\n", isr);
    printf("  IER  (0x04): 0x%08X\n", ier);
    printf("  TDFV (0x0C): 0x%08X (TX Vacancy Counter)\n", tdfv);
    printf("  RDFO (0x1C): 0x%08X (RX Word Occupancy)\n", rdfo);
    printf("  RLR  (0x24): 0x%08X (RX Packet Byte Length)\n\n", rlr);

    printf("[INTERPRETATION & TROUBLESHOOTING]\n");
    
    // Evaluate if the hardware loop has registered complete packets
    if (isr & XLLF_INT_RC_MASK) {
        printf("  -> [RX STATUS]: Receive Complete (RC) is asserted. Packet ready in FIFO.\n");
    } else {
        printf("  -> [RX STATUS]: No 'Receive Complete' flag. Hardware hasn't verified a closed transaction.\n");
    }

    if (rdfo > 0 && rlr == 0) {
        printf("  🚨 [CRITICAL ALERT]: Data is physically inside the FIFO (RDFO = %d words),\n", rdfo);
        printf("     but the Packet Length is reporting 0 bytes (RLR = 0).\n");
        printf("     PROBABLE CAUSE: Your upstream RTL logic is flooding data but failing to pulse TLAST.\n");
        printf("     The hardware cannot close the packet boundaries, forcing an 'always full' lock.\n");
    } else if (rdfo == 0 && (isr & XLLF_INT_RC_MASK)) {
        printf("  ⚠️ [ALERT]: RC flag is high but occupancy is 0. Internal pointers may be desynchronized.\n");
    } else {
        printf("  -> RX Packet Metrics: %d words buffered, packet length metadata is %d bytes.\n", rdfo, rlr);
    }

    // Check for Under/Over run hardware faults
    if (isr & XLLF_INT_RPURE_MASK) {
        printf("  💥 [ERROR]: Receive Under Read Error detected. Software read from an empty hardware state.\n");
    }
    if (isr & XLLF_INT_RPORE_MASK) {
        printf("  💥 [ERROR]: Receive Over Read Error detected. Upstream overflowed the buffer capacity.\n");
    }

    // Cleanup memory allocations
    munmap(map_base, pagesize);
    close(fd);
    return EXIT_SUCCESS;
}
