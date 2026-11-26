#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef AXIS_FIFO_DEFAULT_PATH
#define AXIS_FIFO_DEFAULT_PATH "/dev/axis_fifo0"
#endif

#define ROUTER_BITS 2U
#define ROUTER_POS (32U - ROUTER_BITS)
#define ROUTER_MASK ((1U << ROUTER_BITS) - 1U)
#define ROUTER_OUTPUTS_PORT 1U

#define IOEXPANDER_IO_BITS 16U
#define IOEXPANDER_IO_POS 0U
#define IOEXPANDER_IO_MASK ((1U << IOEXPANDER_IO_BITS) - 1U)

#define IOEXPANDER_ADDR_BITS 3U
#define IOEXPANDER_ADDR_POS (IOEXPANDER_IO_POS + IOEXPANDER_IO_BITS)
#define IOEXPANDER_ADDR_MASK ((1U << IOEXPANDER_ADDR_BITS) - 1U)

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static uint32_t make_ioexpander_word(uint16_t addr, uint16_t value)
{
    return (((uint32_t)addr & IOEXPANDER_ADDR_MASK) << IOEXPANDER_ADDR_POS)
         | (((uint32_t)value & IOEXPANDER_IO_MASK) << IOEXPANDER_IO_POS)
         | (((uint32_t)ROUTER_OUTPUTS_PORT & ROUTER_MASK) << ROUTER_POS);
}

static int sleep_ms(long ms)
{
    if (ms <= 0)
        return 0;

    struct timespec ts = {
        .tv_sec = ms / 1000L,
        .tv_nsec = (ms % 1000L) * 1000000L,
    };

    while (nanosleep(&ts, &ts) < 0) {
        if (errno == EINTR) {
            if (g_stop)
                return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static int write_words_all(int fd, const uint32_t *words, size_t count)
{
    const uint8_t *ptr = (const uint8_t *)words;
    size_t remaining = count * sizeof(*words);

    while (remaining > 0) {
        if (g_stop) {
            errno = ECANCELED;
            return -1;
        }

        ssize_t n = write(fd, ptr, remaining);
        if (n > 0) {
            ptr += (size_t)n;
            remaining -= (size_t)n;
            continue;
        }

        if (n == 0) {
            errno = EIO;
            return -1;
        }

        if (errno == EINTR)
            continue;

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd pfd = {
                .fd = fd,
                .events = POLLOUT,
                .revents = 0,
            };
            int pr = poll(&pfd, 1, 250);
            if (pr < 0 && errno != EINTR)
                return -1;
            if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                errno = EIO;
                return -1;
            }
            continue;
        }

        return -1;
    }

    return 0;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-d device_path] [-n repeat_count] [-w delay_ms]\n"
            "  -d  FIFO path (default: %s)\n"
            "  -n  Repeat count, 0 for infinite until Ctrl+C (default: 1)\n"
            "  -w  Delay in ms between words (default: 10)\n",
            prog, AXIS_FIFO_DEFAULT_PATH);
}

int main(int argc, char **argv)
{
    const char *dev_path = AXIS_FIFO_DEFAULT_PATH;
    unsigned long repeat_count = 1UL;
    long delay_ms = 10L;

    int opt;
    while ((opt = getopt(argc, argv, "d:n:w:h")) != -1) {
        switch (opt) {
        case 'd':
            dev_path = optarg;
            break;
        case 'n':
            repeat_count = strtoul(optarg, NULL, 10);
            break;
        case 'w':
            delay_ms = strtol(optarg, NULL, 10);
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Sequence of test words (bit-walking IO expander outputs on port 1, addr 0x0) */
    uint32_t seq[18];
    size_t seq_len = 0;
    for (unsigned bit = 0; bit < 16U; ++bit) {
        seq[seq_len++] = make_ioexpander_word(0x0U, (uint16_t)(1U << bit))
            | ((1U << (bit >> 1U)) << 19U);
    }
    seq[seq_len++] = make_ioexpander_word(0x0U, 0x0000U);
    seq[seq_len++] = make_ioexpander_word(0x0U, 0xFFFFU);

    int fd = open(dev_path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[-] Failed to open %s: %s\n", dev_path, strerror(errno));
        return 1;
    }

    printf("[+] Writing %zu test words per pass to %s\n", seq_len, dev_path);

    unsigned long pass = 0;
    while (!g_stop && (repeat_count == 0UL || pass < repeat_count)) {
        for (size_t i = 0; i < seq_len && !g_stop; ++i) {
            if (write_words_all(fd, &seq[i], 1U) < 0) {
                if (errno == ECANCELED)
                    break;
                fprintf(stderr, "[-] Write failed at pass %lu word %zu (0x%08" PRIX32 "): %s\n",
                        pass, i, seq[i], strerror(errno));
                close(fd);
                return 1;
            }
            printf("  pass=%lu idx=%02zu word=0x%08" PRIX32 "\n", pass, i, seq[i]);

            if (sleep_ms(delay_ms) < 0)
                break;
        }
        ++pass;
    }

    printf("[+] Done.\n");
    close(fd);
    return 0;
}
