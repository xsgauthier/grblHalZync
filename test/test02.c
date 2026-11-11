#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

// Adjust this path string to match your exact device node name on Debian
#define AXIS_FIFO_DEV_PATH   "/dev/axis_fifo0" 

// Adjust these based on your FIFO depth (e.g. 512, 1024 words)
// Writing in controlled bursts prevents blocking the OS thread indefinitely
#define BURST_SIZE_WORDS     64  

// Stepper Ramp Configurations
#define MIN_DELAY_CYCLES     500       // Top Speed (~200 kHz at 100MHz clock)
#define MAX_DELAY_CYCLES     500000      // Bottom Starting Speed (~200 Hz)
#define STEP_INCREMENT       1500        // Acceleration / Deceleration slope smoothness

volatile sig_atomic_t keep_running = 1;

// Signal handler to safely catch Ctrl-C
void handle_sigint(int sig) {
    keep_running = 0;
}

int main() {
    int fd;
    uint32_t write_buffer[BURST_SIZE_WORDS];
    
    // Register the OS signal trap
    signal(SIGINT, handle_sigint);

    printf("[+] Opening AXI-Stream FIFO driver: %s...\n", AXIS_FIFO_DEV_PATH);
    // Opening with O_WRONLY since we are strictly streaming out to the motor
    fd = open(AXIS_FIFO_DEV_PATH, O_WRONLY);
    if (fd < 0) {
        perror("[-] Failed to open the AXI FIFO device node");
        fprintf(stderr, "[*] Check device permissions or run with sudo.\n");
        return EXIT_FAILURE;
    }
    printf("[+] Device successfully opened.\n");

    uint32_t current_delay = MAX_DELAY_CYCLES;
    int accelerating = 1;  // 1 = Speeding up (decreasing delay), 0 = Slowing down
    uint8_t direction = 1; // 1 = Clockwise (Bit 31 High), 0 = Counter-Clockwise (Bit 31 Low)

    printf("[+] Starting continuous Step/Dir driver ramp pipeline. Press CTRL-C to exit safely.\n");

    while (keep_running) {
        // Construct a batch of step data words inside our stack array
        for (int i = 0; i < BURST_SIZE_WORDS; i++) {
            // Pack our control word: Bit 31 = DIR pin, Bits 30-0 = Delay count
            uint32_t packed_word = (current_delay & 0x7FFFFFFF);
            if (direction) {
                packed_word |= (1U << 31);
            }

            write_buffer[i] = packed_word;

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

        // Push the entire data packet to the driver
        // Note: write() requires the size parameter to be in BYTES, not words.
        ssize_t bytes_sent = write(fd, write_buffer, BURST_SIZE_WORDS * sizeof(uint32_t));
        
        if (bytes_sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // The driver's internal queue buffer is saturated, sleep briefly and retry
                usleep(1000);
            } else if (errno == EINTR) {
                // Interrupted by Ctrl-C signal
                break;
            } else {
                perror("[-] Fatal error occurred while writing to character device");
                break;
            }
        } else if (bytes_sent < (BURST_SIZE_WORDS * sizeof(uint32_t))) {
            // Partial write occurred (FIFO filled up mid-stream)
            // Wind back our delay values slightly or sleep briefly to allow hardware catch-up
            usleep(500);
        } else {
            // A successful full burst write occurred. 
            // Yield slightly to prevent hogging 100% of user CPU core processing cycles
            usleep(100);
        }
    }

    printf("\n[-] Ctrl-C recognized. Closing device node...\n");
    close(fd);
    printf("[+] Done. Stepper driver testing completed cleanly.\n");

    return EXIT_SUCCESS;
}
