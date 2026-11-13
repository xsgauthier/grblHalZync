#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

// Mandated PG080 Register Offsets (relative to the base address)
#define XLLF_ISR_OFFSET    0x00  // Interrupt Status Register
#define XLLF_IER_OFFSET    0x04  // Interrupt Enable Register
#define XLLF_TDFR_OFFSET   0x08  // Transmit Data FIFO Reset
#define XLLF_TDFV_OFFSET   0x0C  // Transmit Data FIFO Vacancy
#define XLLF_TDFD_OFFSET   0x10  // Transmit Data FIFO Data
#define XLLF_TLR_OFFSET    0x14  // Transmit Length Register
#define XLLF_RDFR_OFFSET   0x18  // Receive Data FIFO Reset
#define XLLF_RDFO_OFFSET   0x1C  // Receive Data FIFO Occupancy
#define XLLF_RDFD_OFFSET   0x20  // Receive Data FIFO Data
#define XLLF_RLR_OFFSET    0x24  // Receive Length Register

// Bitmasks for parsing the Interrupt Status Register (ISR)
#define XLLF_INT_RC_MASK    0x04000000  // Receive Complete
#define XLLF_INT_TC_MASK    0x08000000  // Transmit Complete
#define XLLF_INT_TPO_MASK   0x10000000  // Transmit Programmable Empty
#define XLLF_INT_RPO_MASK   0x20000000  // Receive Programmable Empty
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

    // Open /dev/mem with Read/Write permissions
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("Error: Cannot open /dev/mem (Are you running with sudo?)");
        return EXIT_FAILURE;
    }

    // Map the hardware register space with PROT_READ and PROT_WRITE
    void *map_base = mmap(NULL, pagesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base_page);
    if (map_base == MAP_FAILED) {
        perror("Error: Memory map (mmap) failed");
        close(fd);
        return EXIT_FAILURE;
    }

    // Volatile pointer sequence to ensure no compiler optimization cache
    volatile uint32_t *fifo_reg = (volatile uint32_t *)((char *)map_base + page_offset);

    printf("==================================================\n");
    printf("  AXI-Stream FIFO (PG080) Loopback & Verification \n");
    printf("  Target Physical Base Address: 0x%08lX\n", (unsigned long)phys_addr);
    printf("==================================================\n\n");

    // ----------------------------------------------------
    // CRITICAL FIX: Perform Software Reset on TX and RX FIFOs
    // ----------------------------------------------------
    printf("[0] Issuing software reset to TX and RDFR registers...\n");
    fifo_reg[XLLF_TDFR_OFFSET / 4] = 0x000000A5; // Reset Transmit FIFO
    fifo_reg[XLLF_RDFR_OFFSET / 4] = 0x000000A5; // Reset Receive FIFO
    usleep(10000); // Allow pointers to clear (10ms)
    
    // Define test data pattern to transmit
    uint32_t tx_data[] = {0xDEADBEEF, 0x12345678, 0xCAFEBABE, 0x87654321};
    int num_words = sizeof(tx_data) / sizeof(tx_data[0]);
    uint32_t tx_bytes = num_words * sizeof(uint32_t);

    // Check TX FIFO vacancy before writing
    uint32_t tdfv = fifo_reg[XLLF_TDFV_OFFSET / 4];
    printf("[1] Checking TX FIFO Vacancy (TDFV: %u words available)...\n", tdfv);
    if (tdfv < num_words) {
        fprintf(stderr, "Error: Not enough vacancy in TX FIFO to write test packet!\n");
        munmap(map_base, pagesize);
        close(fd);
        return EXIT_FAILURE;
    }

    // Write packet data into Transmit Data FIFO (TDFD)
    printf("[2] Writing %d words (%u bytes) to Transmit FIFO...\n", num_words, tx_bytes);
    for (int i = 0; i < num_words; i++) {
        fifo_reg[XLLF_TDFD_OFFSET / 4] = tx_data[i];
        printf("    -> Wrote 0x%08X to TDFD\n", tx_data[i]);
    }

    // Write packet byte length to Transmit Length Register (TLR) to trigger transmission
    printf("[3] Triggering transmission by writing length to TLR (0x%X)...\n", tx_bytes);
    fifo_reg[XLLF_TLR_OFFSET / 4] = tx_bytes;

    // Poll for Receive Complete (RC) in ISR since it's an internal loopback
    printf("[4] Waiting for loopback packet completion...\n");
    int timeout = 1000000;
    while (--timeout > 0) {
        uint32_t isr = fifo_reg[XLLF_ISR_OFFSET / 4];
        if (isr & XLLF_INT_RC_MASK) {
            break;
        }
    }

    if (timeout <= 0) {
        fprintf(stderr, "❌ Error: Timeout waiting for Receive Complete (RC) flag.\n");
        munmap(map_base, pagesize);
        close(fd);
        return EXIT_FAILURE;
    }

    // Read back occupancy and length metadata (CRITICAL: RDFO must be read before RLR)
    uint32_t rdfo = fifo_reg[XLLF_RDFO_OFFSET / 4];
    uint32_t rlr = 0;
    if (rdfo > 0) {
        rlr = fifo_reg[XLLF_RLR_OFFSET / 4];
    }

    printf("\n[5] Reading back received packet from RX FIFO...\n");
    printf("    -> RDFO (RX Word Occupancy): %u words\n", rdfo);
    printf("    -> RLR  (RX Packet Length):   %u bytes\n", rlr);

    if (rdfo != num_words) {
        fprintf(stderr, "❌ Error: Word count mismatch! Expected %d words, got %u words.\n", num_words, rdfo);
        munmap(map_base, pagesize);
        close(fd);
        return EXIT_FAILURE;
    }

    // Read and verify data from Receive Data FIFO (RDFD)
    int errors = 0;
    for (int i = 0; i < num_words; i++) {
        uint32_t rx_val = fifo_reg[XLLF_RDFD_OFFSET / 4];
        printf("    -> Read 0x%08X | Expected 0x%08X", rx_val, tx_data[i]);
        if (rx_val == tx_data[i]) {
            printf(" [MATCH]\n");
        } else {
            printf(" [MISMATCH]\n");
            errors++;
        }
    }

    printf("\n==================================================\n");
    if (errors == 0 && rlr == tx_bytes) {
        printf("✅ VERIFICATION SUCCESSFUL: Loopback data verified perfectly!\n");
    } else {
        printf("❌ VERIFICATION FAILED: Found %d data mismatches or length mismatch.\n", errors);
    }
    printf("==================================================\n");

    // Cleanup memory allocations
    munmap(map_base, pagesize);
    close(fd);
    return EXIT_SUCCESS;
}
