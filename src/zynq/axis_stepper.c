#include "axis_stepper.h"

#define AXIS_FIFO_DEV_PATH "/dev/axis_fifo0"

#define MIN_DELAY_CYCLES 4200

// Direction and enable fields are ordered Z/Y/X; this test drives X and Y.
#define DELAY_BITS 26
#define DELAY_MASK ((1U << DELAY_BITS) - 1U)

#define NUM_AXES 3
#define NUM_AXES_MASK ((1U << NUM_AXES) - 1U)

#define ENABLE_POS DELAY_BITS
#define DIRECTION_POS (ENABLE_POS+NUM_AXES)

static volatile sig_atomic_t keep_running = 1;

static int write_all(p_axis_stepper_t handle)
{
    const unsigned char *data = (const unsigned char *)handle->buffer;
    size_t bytes_remaining = handle->count * sizeof(handle->buffer[0]);

    while (bytes_remaining > 0) {
        ssize_t bytes_written = write(handle->fd, data, bytes_remaining);
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

static int flush_words(p_axis_stepper_t handle)
{
    if (handle->count == 0)
        return 0;

    int result = write_all(handle);
    if (result == 0)
        handle->count = 0;
    else if (result < 0)
        perror("[-] Failed to write motor FIFO");
    return result;
}

static int queue_word(p_axis_stepper_t handle,
                      uint32_t step_mask,
                      uint32_t dir_mask,
                      uint32_t delay_cycles)
{
    if (delay_cycles > DELAY_MASK) {
        fprintf(stderr, "[-] Delay is outside the FIFO format.\n");
        return -1;
    }

    uint32_t word = (delay_cycles & DELAY_MASK) 
        | ((step_mask & NUM_AXES_MASK) << ENABLE_POS)
        | ((dir_mask & NUM_AXES_MASK) << DIRECTION_POS)
    ;

    handle->buffer[(handle->count)++] = word;
    if (handle->count == BURST_SIZE_WORDS)
        return flush_words(handle);
    return 0;
}

p_axis_stepper_t axis_stepper_initialize(void)
{
    p_axis_stepper_t handle = calloc(1, sizeof(axis_stepper_t));

    printf("[+] Opening axis FIFO: %s...\n", AXIS_FIFO_DEV_PATH);
    handle->fd = open(AXIS_FIFO_DEV_PATH, O_WRONLY);
    if (handle->fd < 0) {
        perror("[-] Failed to open axis FIFO");
        free(handle);
        return NULL;
    }

    handle->write = queue_word;
    handle->flush = flush_words;

    return handle;
}

void axis_stepper_close(p_axis_stepper_t handle)
{
    close(handle->fd);
    free(handle);
}