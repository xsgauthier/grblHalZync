#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define LED_MASK 0x0Fu
#define DEFAULT_UIO_PATH "/dev/uio0"

static volatile sig_atomic_t running = 1;

static void handle_sigint(int sig) {
    (void)sig;
    running = 0;
}

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        if (!running) {
            break;
        }
    }
}

/* active_high_4bit: bit0..bit3 where 1 = LED ON, 0 = LED OFF */
static void write_leds_active_low(volatile uint32_t *reg, uint32_t active_high_4bit) {
    uint32_t active_low_bits = (~active_high_4bit) & LED_MASK;
    uint32_t current = *reg;
    *reg = (current & ~LED_MASK) | active_low_bits;
}

int main(int argc, char *argv[]) {
    const char *uio_path = DEFAULT_UIO_PATH;
    size_t reg_word_offset = 0; /* 32-bit word index (0 = byte offset 0x00) */

    if (argc >= 2) {
        uio_path = argv[1];
    }
    if (argc >= 3) {
        unsigned long byte_off = strtoul(argv[2], NULL, 0);
        reg_word_offset = (size_t)(byte_off / sizeof(uint32_t));
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int fd = open(uio_path, O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open uio device");
        return EXIT_FAILURE;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        page_size = 4096;
    }

    void *map_base = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map_base == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return EXIT_FAILURE;
    }

    volatile uint32_t *regs = (volatile uint32_t *)map_base;
    volatile uint32_t *led_reg = &regs[reg_word_offset];

    /* Pattern tables: 1 = LED ON (converted to 0 on the wire for active-low) */
    const uint32_t sweep[] = {0x1, 0x2, 0x4, 0x8, 0x4, 0x2};
    const uint32_t bounce_fill[] = {0x1, 0x3, 0x7, 0xF, 0xE, 0xC, 0x8, 0x0};
    const uint32_t split_chase[] = {0x9, 0x6, 0x9, 0x6};
    const uint32_t blink[] = {0xA, 0x5, 0xA, 0x5};

    printf("Controlling active-low LEDs on %s (word offset %zu). Press Ctrl+C to exit.\n",
           uio_path, reg_word_offset);

    while (running) {
        /* 1. 4-bit binary up-counter (0..15) */
        for (uint32_t i = 0; i < 16 && running; i++) {
            write_leds_active_low(led_reg, i);
            sleep_ms(160);
        }

        /* 2. Sweep / knight-rider */
        for (size_t i = 0; i < sizeof(sweep) / sizeof(sweep[0]) && running; i++) {
            write_leds_active_low(led_reg, sweep[i]);
            sleep_ms(140);
        }

        /* 3. Progressive fill and drain */
        for (size_t i = 0; i < sizeof(bounce_fill) / sizeof(bounce_fill[0]) && running; i++) {
            write_leds_active_low(led_reg, bounce_fill[i]);
            sleep_ms(150);
        }

        /* 4. Outer / inner pair alternation */
        for (size_t i = 0; i < sizeof(split_chase) / sizeof(split_chase[0]) && running; i++) {
            write_leds_active_low(led_reg, split_chase[i]);
            sleep_ms(220);
        }

        /* 5. Checkerboard blink */
        for (size_t i = 0; i < sizeof(blink) / sizeof(blink[0]) && running; i++) {
            write_leds_active_low(led_reg, blink[i]);
            sleep_ms(180);
        }
    }

    /* Turn all 4 active-low LEDs OFF (write 1s to lower 4 bits) */
    write_leds_active_low(led_reg, 0x0u);

    munmap(map_base, (size_t)page_size);
    close(fd);
    printf("\nExited cleanly and turned LEDs off.\n");
    return EXIT_SUCCESS;
}
