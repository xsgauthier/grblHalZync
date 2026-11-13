#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define UIO_MAP_SIZE 0x2000 // 8 KB memory map range

// Register Offsets (relative to base address)
#define XLLF_ISR_OFFSET  0x00  // Interrupt Status Register
#define XLLF_TDFR_OFFSET 0x08  // Transmit Data FIFO Reset Register
#define XLLF_TDFV_OFFSET 0x0C  // Transmit Data FIFO Vacancy Register
#define XLLF_RDFR_OFFSET 0x18  // Receive Data FIFO Reset Register

// Reset Complete Bitmasks in ISR
#define XLLF_INT_TRC_MASK 0x01000000  // Transmit Reset Complete
#define XLLF_INT_RRC_MASK 0x00800000  // Receive Reset Complete

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s </dev/uio0 or /dev/uio1>\n", argv[0]);
        return EXIT_FAILURE;
    }

    // Open the UIO device node
    int fd = open(argv[1], O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("Error: Cannot open UIO device");
        return EXIT_FAILURE;
    }

    // Map the 8KB register space into userspace via UIO
    void *map_base = mmap(NULL, UIO_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map_base == MAP_FAILED) {
        perror("Error: UIO memory map (mmap) failed");
        close(fd);
        return EXIT_FAILURE;
    }

    volatile uint32_t *fifo_reg = (volatile uint32_t *)map_base;

    printf("==================================================\n");
    printf("     AXI-Stream FIFO (PG080) UIO Reset Test      \n");
    printf("==================================================\n\n");

    // Step 1: Issue software reset magic key to TX and RX reset registers
    printf("[1] Issuing software reset keys (0x000000A5) to TDFR and RDFR...\n");
    fifo_reg[XLLF_TDFR_OFFSET / 4] = 0x000000A5;
    fifo_reg[XLLF_RDFR_OFFSET / 4] = 0x000000A5;

    // Step 2: Poll Interrupt Status Register (ISR) for Transmit and Receive Reset Complete flags
    printf("[2] Waiting for reset completion flags in ISR...\n");
    int timeout = 100000;
    int reset_ok = 0;
    
    while (--timeout > 0) {
        uint32_t isr = fifo_reg[XLLF_ISR_OFFSET / 4];
        if ((isr & XLLF_INT_TRC_MASK) && (isr & XLLF_INT_RRC_MASK)) {
            // Clear the reset complete flags by writing 1 back to them
            fifo_reg[XLLF_ISR_OFFSET / 4] = isr & (XLLF_INT_TRC_MASK | XLLF_INT_RRC_MASK);
            reset_ok = 1;
            break;
        }
    }

    if (!reset_ok) {
        fprintf(stderr, "❌ Error: Timeout waiting for FIFO reset completion (TRC/RRC never asserted).\n");
        munmap(map_base, UIO_MAP_SIZE);
        close(fd);
        return EXIT_FAILURE;
    }

    printf("✅ FIFO reset completed successfully.\n");

    // Step 3: Read Transmit Data FIFO Vacancy (TDFV)
    uint32_t tdfv = fifo_reg[XLLF_TDFV_OFFSET / 4];
    printf("[3] Checking Transmit Data FIFO Vacancy (TDFV): %u words available\n", tdfv);

    if (tdfv > 0) {
        printf("✅ SUCCESS: TDFV is reporting valid vacancy!\n");
    } else {
        printf("⚠️ WARNING: TDFV is still reporting 0. Check stream clocks and stream resets.\n");
    }

    // Cleanup
    munmap(map_base, UIO_MAP_SIZE);
    close(fd);
    return EXIT_SUCCESS;
}