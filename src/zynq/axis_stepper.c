#include "axis_stepper.h"
#include "axis_stepper_diagnostics.h"
#include "logger.h"
#include "object_pool.h"
#include "simulator.h"

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
#define AXIS_FIFO_WRITE_BATCH 64
#define AXIS_STEP_CACHE_BYTES ((size_t)25 * 1024U * 1024U)
#define AXIS_STEP_POOL_CHUNK_BYTES ((size_t)1024U * 1024U)
#define AXIS_FIFO_RETRY_DELAY_NS 10000000L
#define AXIS_DIAGNOSTIC_WAIT_NS 100000000L

#define DELAY_BITS 26
#define DELAY_MASK ((1U << DELAY_BITS) - 1U)
#define NUM_AXES 3
#define NUM_AXES_MASK ((1U << NUM_AXES) - 1U)
#define ENABLE_POS DELAY_BITS
#define DIRECTION_POS (ENABLE_POS + NUM_AXES)
#define MIN_DELAY_CYCLES 4200
#define MAX_DELAY_CYCLES ((1U << DELAY_BITS) - 1U)

#define DELAY_SCALING (50000000U / F_CPU)

typedef struct {
    uint32_t words[AXIS_FIFO_WRITE_BATCH];
    size_t count;
} axis_step_batch_t;

struct axis_stepper_s {
    object_pool_t pool;
    axis_step_batch_t *producer_batch;
    size_t producer_count;
    int stopping;
    int output_started;
    int feed_hold;
    pthread_mutex_t startup_mutex;
    pthread_cond_t startup_ready;
    int last_error;
    int fd;
    const axis_stepper_diagnostics_api_t *diagnostics_api;
    axis_stepper_diagnostics_t diagnostics;
};

static void report_diagnostics(p_axis_stepper_t handle)
{
    if (!handle->diagnostics_api->enabled)
        return;

    size_t pool_used, pool_capacity;
    int buffering;

    object_pool_get_stats(handle->pool, &pool_used, NULL, &pool_capacity);
    pthread_mutex_lock(&handle->startup_mutex);
    buffering = !handle->output_started;
    pthread_mutex_unlock(&handle->startup_mutex);

    handle->diagnostics_api->report(handle->diagnostics, pool_used, pool_capacity, buffering);
}

static void start_output(p_axis_stepper_t handle)
{
    pthread_mutex_lock(&handle->startup_mutex);
    if (!handle->output_started) {
        handle->output_started = 1;
        pthread_cond_broadcast(&handle->startup_ready);
    }
    pthread_mutex_unlock(&handle->startup_mutex);
}

static int wait_for_writer(p_axis_stepper_t handle)
{
    pthread_mutex_lock(&handle->startup_mutex);
    while (!handle->stopping && (!handle->output_started || handle->feed_hold))
        pthread_cond_wait(&handle->startup_ready, &handle->startup_mutex);
    pthread_mutex_unlock(&handle->startup_mutex);
    return 1;
}

static void start_output_if_pool_half_full(p_axis_stepper_t handle)
{
    size_t pool_used, pool_capacity;

    object_pool_get_stats(handle->pool, &pool_used, NULL, &pool_capacity);
    if (pool_used >= pool_capacity / 2 + pool_capacity % 2)
        start_output(handle);
}

static int wait_for_free_batch(p_axis_stepper_t handle)
{
    uint64_t wait_started_ns = handle->diagnostics_api->now_ns();
    int waited = 0;
    void *object = NULL;
    int result;

    while ((result = object_pool_acquire(handle->pool, &object, AXIS_DIAGNOSTIC_WAIT_NS / 1000000U)) == 0) {
        if (!waited) {
            handle->diagnostics_api->producer_wait_begin(handle->diagnostics);
            waited = 1;
        }
        report_diagnostics(handle);
    }

    if (waited)
        handle->diagnostics_api->producer_wait_end(handle->diagnostics,
                               handle->diagnostics_api->now_ns() - wait_started_ns);

    if (result != 1)
        return -1;

    handle->producer_batch = object;
    handle->producer_count = 0;
    handle->producer_batch->count = 0;
    start_output_if_pool_half_full(handle);
    report_diagnostics(handle);
    return 0;
}

static int publish_producer_batch(p_axis_stepper_t handle)
{
    if (!handle->producer_batch || handle->producer_count == 0)
        return 0;

    handle->producer_batch->count = handle->producer_count;
    if (object_pool_publish(handle->pool, handle->producer_batch) < 0)
        return -1;

    handle->producer_batch = NULL;
    handle->producer_count = 0;
    return 0;
}

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
        if (!wait_for_writer(handle))
            return;

        handle->diagnostics_api->write_begin(handle->diagnostics);
        ssize_t bytes_written = write(handle->fd, data, bytes_remaining);
        handle->diagnostics_api->write_end(handle->diagnostics);

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
            uint64_t wait_started_ns = handle->diagnostics_api->now_ns();
            int result = poll(&writable, 1, 100);

            if (result > 0 && (writable.revents & (POLLERR | POLLHUP | POLLNVAL)))
                retry_pause();
            else if (result < 0 && errno != EINTR)
                retry_pause();

            uint64_t wait_ns = handle->diagnostics_api->now_ns() - wait_started_ns;
            handle->diagnostics_api->fifo_wait(handle->diagnostics, wait_ns);
            report_diagnostics(handle);
            continue;
        }

        if (handle->last_error != error) {
            logger_fprintf(stderr, "[-] Axis FIFO write failed: %s; retaining queued steps\n", strerror(error));
            handle->last_error = error;
            handle->diagnostics_api->write_error(handle->diagnostics);
        }
        retry_pause();
        report_diagnostics(handle);
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

    if (handle->stopping) {
        errno = ECANCELED;
        return -1;
    }

    handle->producer_batch->words[handle->producer_count++] = word;
    if (handle->producer_count == AXIS_FIFO_WRITE_BATCH) {
        if (publish_producer_batch(handle) < 0) {
            errno = ECANCELED;
            return -1;
        }
        if (wait_for_free_batch(handle) < 0) {
            errno = ECANCELED;
            return -1;
        }
    }

    return 0;
}

int axis_stepper_flush(p_axis_stepper_t handle)
{
    if (!handle)
        return -1;

    if (!handle->stopping && handle->producer_count) {
        if (publish_producer_batch(handle) < 0)
            return -1;
        int result = wait_for_free_batch(handle);
        return result;
    }
    return 0;
}

void axis_stepper_input_complete(p_axis_stepper_t handle)
{
    if (handle)
        start_output(handle);
}

void axis_stepper_set_feed_hold(p_axis_stepper_t handle, int active)
{
    if (!handle)
        return;

    pthread_mutex_lock(&handle->startup_mutex);
    if (handle->feed_hold != (active != 0)) {
        handle->feed_hold = active != 0;
        if (!handle->feed_hold)
            pthread_cond_broadcast(&handle->startup_ready);
    }
    pthread_mutex_unlock(&handle->startup_mutex);
}

void *axis_stepper_writer_thread(void *arg)
{
    p_axis_stepper_t handle = arg;
    sigset_t blocked_signals;

    sigemptyset(&blocked_signals);
    sigaddset(&blocked_signals, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked_signals, NULL);

    pthread_mutex_lock(&handle->startup_mutex);
    while (!handle->output_started)
        pthread_cond_wait(&handle->startup_ready, &handle->startup_mutex);
    pthread_mutex_unlock(&handle->startup_mutex);

    for (;;) {
        if (handle->feed_hold)
        {
            sim_yield();
            continue;
        }
        axis_step_batch_t *batch = NULL;
        int result = object_pool_take(handle->pool, (void **)&batch);

        if (result <= 0)
            break;

        write_all(handle, batch->words, batch->count);

        object_pool_release(handle->pool, batch);
        handle->diagnostics_api->words_written(handle->diagnostics, batch->count);
        report_diagnostics(handle);
    }

    return NULL;
}

void axis_stepper_stop(p_axis_stepper_t handle)
{
    if (!handle)
        return;

    pthread_mutex_lock(&handle->startup_mutex);
    handle->stopping = 1;
    handle->feed_hold = 0;
    handle->output_started = 1;
    pthread_cond_broadcast(&handle->startup_ready);
    pthread_mutex_unlock(&handle->startup_mutex);
    publish_producer_batch(handle);
    object_pool_stop(handle->pool);
}

p_axis_stepper_t axis_stepper_initialize(void)
{
    p_axis_stepper_t handle = calloc(1, sizeof(*handle));
    void *producer_batch = NULL;

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

    handle->diagnostics_api = axis_stepper_diagnostics_get_api();
    handle->diagnostics = handle->diagnostics_api->create();
    if (!handle->diagnostics) {
        logger_fprintf(stderr, "[-] Failed to initialize axis FIFO diagnostics\n");
        close(handle->fd);
        free(handle);
        return NULL;
    }

    handle->pool = object_pool_create(sizeof(axis_step_batch_t), AXIS_STEP_CACHE_BYTES,
                                      AXIS_STEP_POOL_CHUNK_BYTES);
    if (!handle->pool) {
        logger_fprintf(stderr, "[-] Failed to allocate axis step cache: %s\n", strerror(errno));
        axis_stepper_diagnostics_destroy(handle->diagnostics);
        close(handle->fd);
        free(handle);
        return NULL;
    }

    if (object_pool_acquire(handle->pool, &producer_batch, 0) != 1) {
        logger_fprintf(stderr, "[-] Failed to acquire initial axis step batch\n");
        object_pool_destroy(handle->pool);
        axis_stepper_diagnostics_destroy(handle->diagnostics);
        close(handle->fd);
        free(handle);
        return NULL;
    }

    handle->producer_batch = producer_batch;
    size_t pool_used, pool_capacity;
    object_pool_get_stats(handle->pool, &pool_used, NULL, &pool_capacity);
    handle->diagnostics_api->queue_depth(handle->diagnostics, pool_used);

    int error = pthread_mutex_init(&handle->startup_mutex, NULL);
    if (error) {
        logger_fprintf(stderr, "[-] Failed to initialize stepper startup mutex: %s\n", strerror(error));
        object_pool_destroy(handle->pool);
        handle->diagnostics_api->destroy(handle->diagnostics);
        close(handle->fd);
        free(handle);
        return NULL;
    }

    error = pthread_cond_init(&handle->startup_ready, NULL);
    if (error) {
        logger_fprintf(stderr, "[-] Failed to initialize stepper startup condition: %s\n", strerror(error));
        pthread_mutex_destroy(&handle->startup_mutex);
        object_pool_destroy(handle->pool);
        handle->diagnostics_api->destroy(handle->diagnostics);
        close(handle->fd);
        free(handle);
        return NULL;
    }

    start_output_if_pool_half_full(handle);
    logger_fprintf(stdout, "[+] Axis step cache: %zu bytes, %zu batches of %u words\n",
                   object_pool_memory_bytes(handle->pool), pool_capacity, AXIS_FIFO_WRITE_BATCH);

    return handle;
}

void axis_stepper_close(p_axis_stepper_t handle)
{
    if (!handle)
        return;

    close(handle->fd);
    object_pool_destroy(handle->pool);
    handle->diagnostics_api->destroy(handle->diagnostics);
    pthread_cond_destroy(&handle->startup_ready);
    pthread_mutex_destroy(&handle->startup_mutex);
    free(handle);
}