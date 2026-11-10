#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>

// Change this base address to match your Vivado Address Editor assignment!
#define AXI_FIFO_BASE_ADDR   0x43C00000 
#define MAP_SIZE             0x1000

// Xilinx AXI-Stream FIFO Registers Offsets
#define REG_TDFV             (0x0C / 4)  // Transmit Vacancy (Read Only)
#define REG_TDFD             (0x10 / 4)  // Transmit Data Write Window
#define REG_TLF              (0x14 / 4)  // Transmit Length Register (Kickoff)

// Stepper Ramp Configurations
#define MIN_DELAY_CYCLES     50000       // Top Speed (~2 kHz at 100MHz clock)
#define MAX_DELAY_CYCLES     500000      // Bottom Starting Speed (~200 Hz)
#define STEP_INCREMENT       1500        // Acceleration / Deceleration slope smoothness

volatile sig_atomic_t keep_running = 1;

// Signal handler to safely catch Ctrl-C
void handle_sigint(int sig) {
    keep_running = 0;
}

int main() {
    int fd;
    volatile uint32_t *fifo_regs;
    
    // Register the OS signal trap
    signal(SIGINT, handle_sigint);

    printf("[+] Opening /dev/mem to access physical registers...\n");
    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("[-] Failed to open /dev/mem. Are you running as root (sudo)?");
        return EXIT_FAILURE;
    }

    // Map the physical memory block space directly into our user process space
    fifo_regs = (volatile uint32_t *)mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, AXI_FIFO_BASE_ADDR);
    if (fifo_regs == MAP_FAILED) {
        perror("[-] Memory mapping failed");
        close(fd);
        return EXIT_FAILURE;
    }
    printf("[+] Memory successfully mapped at virtual pointer: %p\n", (void*)fifo_regs);

    uint32_t current_delay = MAX_DELAY_CYCLES;
    int accelerating = 1; // 1 = Speeding up (decreasing delay), 0 = Slowing down (increasing delay)
    uint8_t direction = 1; // 1 = Clockwise (Bit 31 High), 0 = Counter-Clockwise (Bit 31 Low)

    printf("[+] Starting continuous Step/Dir ramp pipeline. Press CTRL-C to exit safely.\n");

    while (keep_running) {
        // Read how many free slots currently exist inside the Xilinx FIFO buffer
        uint32_t vacancy = fifo_regs[REG_TDFV];
        printf("[+] FIFO Vacancy: %u slots available\n", vacancy);

        if (vacancy > 10) { 
            // Calculate a burst packet size to send to the FIFO
            uint32_t burst_size = (vacancy > 64) ? 64 : vacancy - 4;
            uint32_t bytes_written = burst_size * 4;

            for (uint32_t i = 0; i < burst_size; i++) {
                // Pack our control word: Bit 31 represents DIR pin, Bits 30-0 represent Delay counts
                uint32_t packed_word = (current_delay & 0x7FFFFFFF);
                if (direction) {
                    packed_word |= (1U << 31);
                }
                // Write directly to the Transmit FIFO port
                fifo_regs[REG_TDFD] = packed_word;

                // Process the acceleration/deceleration triangle logic math
                if (accelerating) {
                    if (current_delay > (MIN_DELAY_CYCLES + STEP_INCREMENT)) {
                        current_delay -= STEP_INCREMENT; // Speed up
                    } else {
                        accelerating = 0; // Hit top speed inflection point
                    }
                } else {
                    if (current_delay < (MAX_DELAY_CYCLES - STEP_INCREMENT)) {
                        current_delay += STEP_INCREMENT; // Slow down
                    } else {
                        accelerating = 1; // Hit bottom speed inflection point
                        direction = !direction; // Flip physical motor direction state on every ramp reset
                    }
                }
            }

            // CRUCIAL: Kick off the AXI-Stream transaction by writing the byte count into TLF
            fifo_regs[REG_TLF] = bytes_written;
        } else {
            // FIFO is highly saturated, sleep briefly (~500 µs) to yield CPU cycles to Debian scheduler
            usleep(500);
        }
    }

    printf("\n[-] Ctrl-C recognized. Cleaning up allocations and closing mapping...\n");
    munmap((void*)fifo_regs, MAP_SIZE);
    close(fd);
    printf("[+] Done. Stepper motor controller stopped cleanly.\n");

    return EXIT_SUCCESS;
}
