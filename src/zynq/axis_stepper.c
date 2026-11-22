#include "axis_stepper.h"
#include "logger.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define AXIS_FIFO_DEV_PATH "/dev/axis_fifo0"
#define AXIS_STEP_QUEUE_CAPACITY 4096
#define AXIS_FIFO_WRITE_BATCH 64
#define AXIS_FIFO_RETRY_DELAY_NS 10000000L

#define DELAY_BITS 26
#define DELAY_MASK ((1U << DELAY_BITS) - 1U)
#define NUM_AXES 3
#define NUM_AXES_MASK ((1U << NUM_AXES) - 1U)
#define ENABLE_POS DELAY_BITS
#define DIRECTION_POS (ENABLE_POS + NUM_AXES)
#define MIN_DELAY_CYCLES 4200
#define MAX_DELAY_CYCLES ((1U << DELAY_BITS) - 1U)

#define DELAY_SCALING (50000000U / F_CPU)

struct axis_stepper_s {
    uint32_t words[AXIS_STEP_QUEUE_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t data_ready;
    pthread_cond_t space_ready;
    int stopping;
    int last_error;
    int fd;
};

static void retry_pause(void)
{
    const struct timespec pause = { .tv_sec = 0, .tv_nsec = AXIS_FIFO_RETRY_DELAY_NS };

    while (nanosleep(&pause, NULL) < 0 && errno == EINTR)
        ;
}

static void write_all(p_axis_stepper_t handle, const uint32_t *words, size_t count)
{
    const unsigned char *data = (const unsigned char *)words;
    size_t bytes_remaining = count * sizeof(*words);

    while (bytes_remaining > 0) {
        ssize_t bytes_written = write(handle->fd, data, bytes_remaining);

        if (bytes_written > 0) {
            data += bytes_written;
            bytes_remaining -= (size_t)bytes_written;
            handle->last_error = 0;
            continue;
        }

        int error = bytes_written == 0 ? EIO : errno;
        if (error == EINTR)
            continue;

        if (error == EAGAIN || error == EWOULDBLOCK) {
            struct pollfd writable = { .fd = handle->fd, .events = POLLOUT };
            int result = poll(&writable, 1, 100);

            if (result > 0 && (writable.revents & (POLLERR | POLLHUP | POLLNVAL)))
                retry_pause();
            else if (result < 0 && errno != EINTR)
                retry_pause();
            continue;
        }

        if (handle->last_error != error) {
            logger_fprintf(stderr, "[-] Axis FIFO write failed: %s; retaining queued steps\n", strerror(error));
            handle->last_error = error;
        }
        retry_pause();
    }
}

int axis_stepper_enqueue(p_axis_stepper_t handle,
                         uint32_t step_mask,
                         uint32_t dir_mask,
                         uint32_t delay_cycles)
{
    if (!handle)
        return -1;

    delay_cycles *= DELAY_SCALING;
    if (delay_cycles > MAX_DELAY_CYCLES) {
        logger_fprintf(stderr, "[-] WARN: Delay is outside the FIFO format, clipping at %u.\n", MAX_DELAY_CYCLES);
        delay_cycles = MAX_DELAY_CYCLES;
    }
    if (delay_cycles < MIN_DELAY_CYCLES)
        delay_cycles = MIN_DELAY_CYCLES;

    uint32_t word = (delay_cycles & DELAY_MASK)
        | ((step_mask & NUM_AXES_MASK) << ENABLE_POS)
        | ((dir_mask & NUM_AXES_MASK) << DIRECTION_POS);

    pthread_mutex_lock(&handle->mutex);
    while (handle->count == AXIS_STEP_QUEUE_CAPACITY && !handle->stopping)
        pthread_cond_wait(&handle->space_ready, &handle->mutex);

    if (handle->stopping) {
        pthread_mutex_unlock(&handle->mutex);
        errno = ECANCELED;
        return -1;
    }

    handle->words[handle->tail] = word;
    handle->tail = (handle->tail + 1) % AXIS_STEP_QUEUE_CAPACITY;
    handle->count++;
    pthread_cond_signal(&handle->data_ready);
    pthread_mutex_unlock(&handle->mutex);

    return 0;
}

int axis_stepper_flush(p_axis_stepper_t handle)
{
    return handle ? 0 : -1;
}

void *axis_stepper_writer_thread(void *arg)
{
    p_axis_stepper_t handle = arg;
    sigset_t blocked_signals;

    sigemptyset(&blocked_signals);
    sigaddset(&blocked_signals, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked_signals, NULL);

    for (;;) {
        uint32_t batch[AXIS_FIFO_WRITE_BATCH];
        size_t batch_count;

        pthread_mutex_lock(&handle->mutex);
        while (handle->count == 0 && !handle->stopping)
            pthread_cond_wait(&handle->data_ready, &handle->mutex);

        if (handle->count == 0 && handle->stopping) {
            pthread_mutex_unlock(&handle->mutex);
            break;
        }

        batch_count = handle->count;
        if (batch_count > AXIS_FIFO_WRITE_BATCH)
            batch_count = AXIS_FIFO_WRITE_BATCH;
        if (batch_count > AXIS_STEP_QUEUE_CAPACITY - handle->head)
            batch_count = AXIS_STEP_QUEUE_CAPACITY - handle->head;
        memcpy(batch, handle->words + handle->head, batch_count * sizeof(*batch));
        pthread_mutex_unlock(&handle->mutex);

        write_all(handle, batch, batch_count);

        pthread_mutex_lock(&handle->mutex);
        handle->head = (handle->head + batch_count) % AXIS_STEP_QUEUE_CAPACITY;
        handle->count -= batch_count;
        pthread_cond_broadcast(&handle->space_ready);
        pthread_mutex_unlock(&handle->mutex);
    }

    return NULL;
}

void axis_stepper_stop(p_axis_stepper_t handle)
{
    if (!handle)
        return;

    pthread_mutex_lock(&handle->mutex);
    handle->stopping = 1;
    pthread_cond_broadcast(&handle->data_ready);
    pthread_cond_broadcast(&handle->space_ready);
    pthread_mutex_unlock(&handle->mutex);
}

p_axis_stepper_t axis_stepper_initialize(void)
{
    p_axis_stepper_t handle = calloc(1, sizeof(*handle));
    int error;

    if (!handle) {
        logger_fprintf(stderr, "[-] Failed to allocate axis FIFO state\n");
        return NULL;
    }

    logger_fprintf(stdout, "[+] Opening axis FIFO: %s...\n", AXIS_FIFO_DEV_PATH);
    handle->fd = open(AXIS_FIFO_DEV_PATH, O_WRONLY);
    if (handle->fd < 0) {
        logger_fprintf(stderr, "[-] Failed to open axis FIFO: %s\n", strerror(errno));
        free(handle);
        return NULL;
    }

    int flags = fcntl(handle->fd, F_GETFL, 0);
    if (flags < 0 || fcntl(handle->fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        logger_fprintf(stderr, "[-] Failed to enable nonblocking axis FIFO writes: %s\n", strerror(errno));
        close(handle->fd);
        free(handle);
        return NULL;
    }

    error = pthread_mutex_init(&handle->mutex, NULL);
    if (error) {
        logger_fprintf(stderr, "[-] Failed to initialize axis FIFO queue mutex: %s\n", strerror(error));
        close(handle->fd);
        free(handle);
        return NULL;
    }

    error = pthread_cond_init(&handle->data_ready, NULL);
    if (error) {
        logger_fprintf(stderr, "[-] Failed to initialize axis FIFO queue condition: %s\n", strerror(error));
        pthread_mutex_destroy(&handle->mutex);
        close(handle->fd);
        free(handle);
        return NULL;
    }

    error = pthread_cond_init(&handle->space_ready, NULL);
    if (error) {
        logger_fprintf(stderr, "[-] Failed to initialize axis FIFO queue condition: %s\n", strerror(error));
        pthread_cond_destroy(&handle->data_ready);
        pthread_mutex_destroy(&handle->mutex);
        close(handle->fd);
        free(handle);
        return NULL;
    }

    return handle;
}

void axis_stepper_close(p_axis_stepper_t handle)
{
    if (!handle)
        return;

    close(handle->fd);
    pthread_cond_destroy(&handle->space_ready);
    pthread_cond_destroy(&handle->data_ready);
    pthread_mutex_destroy(&handle->mutex);
    free(handle);
}