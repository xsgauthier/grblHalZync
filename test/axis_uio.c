#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define UIO_MAP_SIZE 0x2000 // 8 KB range matching the device tree

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

    // Map the 8KB register space directly from the UIO driver
    void *map_base = mmap(NULL, UIO_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map_base == MAP_FAILED) {
        perror("Error: UIO memory map (mmap) failed");
        close(fd);
        return EXIT_FAILURE;
    }

    volatile uint32_t *fifo_reg = (volatile uint32_t *)map_base;

    // Read TDFV (Transmit Data FIFO Vacancy at offset 0x0C)
    uint32_t tdfv = fifo_reg[0x0C / 4];
    printf("Successfully read TDFV via UIO: %u words available\n", tdfv);

    // Cleanup
    munmap(map_base, UIO_MAP_SIZE);
    close(fd);
    return EXIT_SUCCESS;
}
