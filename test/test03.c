#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define AXIS_X_FIFO_DEV_PATH "/dev/axis_fifo0"
#define AXIS_Y_FIFO_DEV_PATH "/dev/axis_fifo1"

#define BURST_SIZE_WORDS 64
#define MIN_DELAY_CYCLES 5000
#define CIRCLE_RADIUS_STEPS 1000

#define DELAY_BITS 30
#define DELAY_MASK ((1U << DELAY_BITS) - 1U)
#define DIRECTION_BIT 31
#define DIRECTION_MASK (1U << DIRECTION_BIT)
#define ENABLE_BIT 30
#define ENABLE_MASK (1U << ENABLE_BIT)

static volatile sig_atomic_t keep_running = 1;

static void handle_sigint(int signal_number)
{
    (void)signal_number;
    keep_running = 0;
}

static int write_all(int fd, const uint32_t *words, size_t count)
{
    const unsigned char *data = (const unsigned char *)words;
    size_t bytes_remaining = count * sizeof(*words);

    while (bytes_remaining > 0) {
        ssize_t bytes_written = write(fd, data, bytes_remaining);
        if (bytes_written > 0) {
            data += bytes_written;
            bytes_remaining -= (size_t)bytes_written;
        } else if (bytes_written < 0 && errno == EINTR) {
            if (!keep_running)
                return 1;
        } else if (bytes_written < 0 &&
                   (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000000 };
            if (!keep_running)
                return 1;
            nanosleep(&pause, NULL);
        } else if (bytes_written == 0) {
            errno = EIO;
            return -1;
        } else {
            return -1;
        }
    }

    return 0;
}

static int flush_words(int fd, uint32_t *buffer, size_t *count)
{
    if (*count == 0)
        return 0;

    int result = write_all(fd, buffer, *count);
    if (result == 0)
        *count = 0;
    else if (result < 0)
        perror("[-] Failed to write motor FIFO");
    return result;
}

static int queue_step(int fd, uint32_t *buffer, size_t *count,
                      int step_delta, double delay_cycles)
{
    if (step_delta == 0)
        return 0;
    if (step_delta < -1 || step_delta > 1 ||
        !isfinite(delay_cycles) || delay_cycles < 0.0 ||
        delay_cycles > DELAY_MASK) {
        fprintf(stderr, "[-] Circle step or delay is outside the FIFO format.\n");
        return -1;
    }

    uint32_t delay = (uint32_t)llround(delay_cycles);
    if (delay == 0)
        delay = 1;

    uint32_t word = (delay & DELAY_MASK) | ENABLE_MASK;
    if (step_delta > 0)
        word |= DIRECTION_MASK;

    buffer[(*count)++] = word;
    if (*count == BURST_SIZE_WORDS)
        return flush_words(fd, buffer, count);
    return 0;
}

int main(void)
{
    uint32_t x_buffer[BURST_SIZE_WORDS];
    uint32_t y_buffer[BURST_SIZE_WORDS];
    size_t x_count = 0;
    size_t y_count = 0;
    int x_fd;
    int y_fd;
    int result = EXIT_SUCCESS;
    const double radius = CIRCLE_RADIUS_STEPS;
    const double two_pi = 2.0 * acos(-1.0);
    const size_t sample_count = (size_t)ceil(4.0 * two_pi * radius);
    double x_last_step_angle = 0.0;
    double y_last_step_angle = 0.0;
    long x_position = lround(radius);
    long y_position = 0;

    signal(SIGINT, handle_sigint);

    printf("[+] Opening X axis FIFO: %s...\n", AXIS_X_FIFO_DEV_PATH);
    x_fd = open(AXIS_X_FIFO_DEV_PATH, O_WRONLY);
    if (x_fd < 0) {
        perror("[-] Failed to open X axis FIFO");
        return EXIT_FAILURE;
    }

    printf("[+] Opening Y axis FIFO: %s...\n", AXIS_Y_FIFO_DEV_PATH);
    y_fd = open(AXIS_Y_FIFO_DEV_PATH, O_WRONLY);
    if (y_fd < 0) {
        perror("[-] Failed to open Y axis FIFO");
        close(x_fd);
        return EXIT_FAILURE;
    }

    printf("[+] Drawing a %d-step-radius circle at constant tangential speed. "
           "Press CTRL-C to stop.\n", CIRCLE_RADIUS_STEPS);

    while (keep_running && result == EXIT_SUCCESS) {
        for (size_t sample = 1; sample <= sample_count && keep_running; sample++) {
            double angle = two_pi * (double)sample / (double)sample_count;
            long next_x;
            long next_y;

            if (sample == sample_count) {
                next_x = lround(radius);
                next_y = 0;
            } else {
                next_x = lround(radius * cos(angle));
                next_y = lround(radius * sin(angle));
            }

            int x_delta = (int)(next_x - x_position);
            int y_delta = (int)(next_y - y_position);

            if (x_delta != 0) {
                double angle_delta = angle - x_last_step_angle;
                if (angle_delta <= 0.0)
                    angle_delta += two_pi;
                result = queue_step(x_fd, x_buffer, &x_count, x_delta,
                                    angle_delta * radius * MIN_DELAY_CYCLES);
                x_last_step_angle = angle;
            }
            if (result != 0)
                break;

            if (y_delta != 0) {
                double angle_delta = angle - y_last_step_angle;
                if (angle_delta <= 0.0)
                    angle_delta += two_pi;
                result = queue_step(y_fd, y_buffer, &y_count, y_delta,
                                    angle_delta * radius * MIN_DELAY_CYCLES);
                y_last_step_angle = angle;
            }
            if (result != 0)
                break;

            x_position = next_x;
            y_position = next_y;
        }

        if (result == EXIT_SUCCESS && keep_running) {
            result = flush_words(x_fd, x_buffer, &x_count);
            if (result == 0)
                result = flush_words(y_fd, y_buffer, &y_count);
        }
    }

    int interrupted_write = result == 1 && !keep_running;
    if (interrupted_write)
        result = EXIT_SUCCESS;

    if (result == EXIT_SUCCESS) {
        if (!interrupted_write) {
            int flush_result = flush_words(x_fd, x_buffer, &x_count);
            if (flush_result < 0)
                result = EXIT_FAILURE;
            flush_result = flush_words(y_fd, y_buffer, &y_count);
            if (flush_result < 0)
                result = EXIT_FAILURE;
        }
    } else {
        result = EXIT_FAILURE;
    }

    printf("\n[-] Stopping circle stream and closing FIFO devices.\n");
    close(y_fd);
    close(x_fd);
    return result;
}
