#include "axis_stepper_diagnostics.h"
#include "logger.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define DIAGNOSTIC_INTERVAL_NS 1000000000ULL

struct axis_stepper_diagnostics_s {
    pthread_mutex_t mutex;
    uint64_t timestamp_ns;
    int report_started;
    uint64_t words_written;
    uint64_t producer_waits;
    uint64_t producer_wait_ns;
    uint64_t producer_wait_max_ns;
    uint64_t fifo_eagain;
    uint64_t fifo_wait_ns;
    uint64_t fifo_wait_max_ns;
    uint64_t fifo_write_calls;
    uint64_t fifo_write_ns;
    uint64_t fifo_write_max_ns;
    uint64_t fifo_slow_writes;
    uint64_t write_errors;
    size_t queue_high_water;
    uint64_t writer_syscall_started_ns;
    int writer_syscall_active;
};

static struct axis_stepper_diagnostics_s dummy_context;

static axis_stepper_diagnostics_t dummy_create(void)
{
    return &dummy_context;
}

static void dummy_destroy(axis_stepper_diagnostics_t diagnostics)
{
    (void)diagnostics;
}

static uint64_t dummy_now_ns(void)
{
    return 0;
}

static void dummy_queue_depth(axis_stepper_diagnostics_t diagnostics, size_t count)
{
    (void)diagnostics;
    (void)count;
}

static void dummy_producer_wait_begin(axis_stepper_diagnostics_t diagnostics)
{
    (void)diagnostics;
}

static void dummy_producer_wait_end(axis_stepper_diagnostics_t diagnostics, uint64_t wait_ns)
{
    (void)diagnostics;
    (void)wait_ns;
}

static void dummy_fifo_wait(axis_stepper_diagnostics_t diagnostics, uint64_t wait_ns)
{
    (void)diagnostics;
    (void)wait_ns;
}

static void dummy_write_begin(axis_stepper_diagnostics_t diagnostics)
{
    (void)diagnostics;
}

static void dummy_write_end(axis_stepper_diagnostics_t diagnostics)
{
    (void)diagnostics;
}

static void dummy_write_error(axis_stepper_diagnostics_t diagnostics)
{
    (void)diagnostics;
}

static void dummy_words_written(axis_stepper_diagnostics_t diagnostics, size_t count)
{
    (void)diagnostics;
    (void)count;
}

static void dummy_report(axis_stepper_diagnostics_t diagnostics,
                         size_t queue_count,
                         size_t queue_capacity,
                         int buffering)
{
    (void)diagnostics;
    (void)queue_count;
    (void)queue_capacity;
    (void)buffering;
}

uint64_t axis_stepper_diagnostics_now_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;

    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

axis_stepper_diagnostics_t axis_stepper_diagnostics_create(void)
{
    axis_stepper_diagnostics_t diagnostics = calloc(1, sizeof(*diagnostics));

    if (!diagnostics)
        return NULL;

    if (pthread_mutex_init(&diagnostics->mutex, NULL) != 0) {
        free(diagnostics);
        return NULL;
    }

    diagnostics->timestamp_ns = axis_stepper_diagnostics_now_ns();
    return diagnostics;
}

void axis_stepper_diagnostics_destroy(axis_stepper_diagnostics_t diagnostics)
{
    if (!diagnostics)
        return;

    pthread_mutex_destroy(&diagnostics->mutex);
    free(diagnostics);
}

void axis_stepper_diagnostics_queue_depth(axis_stepper_diagnostics_t diagnostics, size_t count)
{
    pthread_mutex_lock(&diagnostics->mutex);
    if (count > diagnostics->queue_high_water)
        diagnostics->queue_high_water = count;
    pthread_mutex_unlock(&diagnostics->mutex);
}

void axis_stepper_diagnostics_producer_wait_begin(axis_stepper_diagnostics_t diagnostics)
{
    pthread_mutex_lock(&diagnostics->mutex);
    diagnostics->producer_waits++;
    pthread_mutex_unlock(&diagnostics->mutex);
}

void axis_stepper_diagnostics_producer_wait_end(axis_stepper_diagnostics_t diagnostics, uint64_t wait_ns)
{
    pthread_mutex_lock(&diagnostics->mutex);
    diagnostics->producer_wait_ns += wait_ns;
    if (wait_ns > diagnostics->producer_wait_max_ns)
        diagnostics->producer_wait_max_ns = wait_ns;
    pthread_mutex_unlock(&diagnostics->mutex);
}

void axis_stepper_diagnostics_fifo_wait(axis_stepper_diagnostics_t diagnostics, uint64_t wait_ns)
{
    pthread_mutex_lock(&diagnostics->mutex);
    diagnostics->fifo_eagain++;
    diagnostics->fifo_wait_ns += wait_ns;
    if (wait_ns > diagnostics->fifo_wait_max_ns)
        diagnostics->fifo_wait_max_ns = wait_ns;
    pthread_mutex_unlock(&diagnostics->mutex);
}

void axis_stepper_diagnostics_write_begin(axis_stepper_diagnostics_t diagnostics)
{
    pthread_mutex_lock(&diagnostics->mutex);
    diagnostics->writer_syscall_started_ns = axis_stepper_diagnostics_now_ns();
    diagnostics->writer_syscall_active = 1;
    pthread_mutex_unlock(&diagnostics->mutex);
}

void axis_stepper_diagnostics_write_end(axis_stepper_diagnostics_t diagnostics)
{
    uint64_t now = axis_stepper_diagnostics_now_ns();

    pthread_mutex_lock(&diagnostics->mutex);
    uint64_t write_ns = now - diagnostics->writer_syscall_started_ns;
    diagnostics->writer_syscall_active = 0;
    diagnostics->fifo_write_calls++;
    diagnostics->fifo_write_ns += write_ns;
    if (write_ns > diagnostics->fifo_write_max_ns)
        diagnostics->fifo_write_max_ns = write_ns;
    if (write_ns >= 1000000ULL)
        diagnostics->fifo_slow_writes++;
    pthread_mutex_unlock(&diagnostics->mutex);
}

void axis_stepper_diagnostics_write_error(axis_stepper_diagnostics_t diagnostics)
{
    pthread_mutex_lock(&diagnostics->mutex);
    diagnostics->write_errors++;
    pthread_mutex_unlock(&diagnostics->mutex);
}

void axis_stepper_diagnostics_words_written(axis_stepper_diagnostics_t diagnostics, size_t count)
{
    pthread_mutex_lock(&diagnostics->mutex);
    diagnostics->words_written += count;
    pthread_mutex_unlock(&diagnostics->mutex);
}

void axis_stepper_diagnostics_report(axis_stepper_diagnostics_t diagnostics,
                                     size_t queue_count,
                                     size_t queue_capacity,
                                     int buffering)
{
    uint64_t now = axis_stepper_diagnostics_now_ns();
    uint64_t words_written, producer_waits, producer_wait_ns, producer_wait_max_ns;
    uint64_t fifo_eagain, fifo_wait_ns, fifo_wait_max_ns;
    uint64_t fifo_write_calls, fifo_write_ns, fifo_write_max_ns, fifo_slow_writes;
    uint64_t write_errors, writer_active_ns;
    size_t queue_high_water;
    int writer_active;
    unsigned pool_percent;

    pool_percent = queue_capacity ? (unsigned)((queue_count * 100U) / queue_capacity) : 0;

    pthread_mutex_lock(&diagnostics->mutex);
    if (diagnostics->report_started && now - diagnostics->timestamp_ns < DIAGNOSTIC_INTERVAL_NS) {
        pthread_mutex_unlock(&diagnostics->mutex);
        return;
    }

    diagnostics->timestamp_ns = now;
    diagnostics->report_started = 1;
    words_written = diagnostics->words_written;
    producer_waits = diagnostics->producer_waits;
    producer_wait_ns = diagnostics->producer_wait_ns;
    producer_wait_max_ns = diagnostics->producer_wait_max_ns;
    fifo_eagain = diagnostics->fifo_eagain;
    fifo_wait_ns = diagnostics->fifo_wait_ns;
    fifo_wait_max_ns = diagnostics->fifo_wait_max_ns;
    fifo_write_calls = diagnostics->fifo_write_calls;
    fifo_write_ns = diagnostics->fifo_write_ns;
    fifo_write_max_ns = diagnostics->fifo_write_max_ns;
    fifo_slow_writes = diagnostics->fifo_slow_writes;
    write_errors = diagnostics->write_errors;
    queue_high_water = diagnostics->queue_high_water;
    writer_active = diagnostics->writer_syscall_active;
    writer_active_ns = writer_active ? now - diagnostics->writer_syscall_started_ns : 0;

    diagnostics->words_written = 0;
    diagnostics->producer_waits = 0;
    diagnostics->producer_wait_ns = 0;
    diagnostics->producer_wait_max_ns = 0;
    diagnostics->fifo_eagain = 0;
    diagnostics->fifo_wait_ns = 0;
    diagnostics->fifo_wait_max_ns = 0;
    diagnostics->fifo_write_calls = 0;
    diagnostics->fifo_write_ns = 0;
    diagnostics->fifo_write_max_ns = 0;
    diagnostics->fifo_slow_writes = 0;
    diagnostics->write_errors = 0;
    diagnostics->queue_high_water = queue_count;
    pthread_mutex_unlock(&diagnostics->mutex);

    if (buffering || words_written || producer_waits || fifo_eagain || fifo_write_calls || write_errors) {
        logger_fprintf(stderr,
                           "[stepper] phase=%s pool=%zu/%zu (%u%%) high=%zu words=%llu producer_waits=%llu producer_wait_ms=%llu producer_wait_max_ms=%llu fifo_eagain=%llu fifo_wait_ms=%llu fifo_wait_max_ms=%llu write_calls=%llu write_ms=%llu write_max_ms=%llu write_slow=%llu writer_active_ms=%llu errors=%llu\n",
                       buffering ? "buffering" : "streaming",
                       queue_count, queue_capacity, pool_percent, queue_high_water,
                       (unsigned long long)words_written,
                       (unsigned long long)producer_waits,
                       (unsigned long long)(producer_wait_ns / 1000000ULL),
                       (unsigned long long)(producer_wait_max_ns / 1000000ULL),
                       (unsigned long long)fifo_eagain,
                       (unsigned long long)(fifo_wait_ns / 1000000ULL),
                       (unsigned long long)(fifo_wait_max_ns / 1000000ULL),
                       (unsigned long long)fifo_write_calls,
                       (unsigned long long)(fifo_write_ns / 1000000ULL),
                       (unsigned long long)(fifo_write_max_ns / 1000000ULL),
                       (unsigned long long)fifo_slow_writes,
                       (unsigned long long)(writer_active_ns / 1000000ULL),
                       (unsigned long long)write_errors);
    }
}

const axis_stepper_diagnostics_api_t axis_stepper_diagnostics_default_api = {
    .enabled = 1,
    .create = axis_stepper_diagnostics_create,
    .destroy = axis_stepper_diagnostics_destroy,
    .now_ns = axis_stepper_diagnostics_now_ns,
    .queue_depth = axis_stepper_diagnostics_queue_depth,
    .producer_wait_begin = axis_stepper_diagnostics_producer_wait_begin,
    .producer_wait_end = axis_stepper_diagnostics_producer_wait_end,
    .fifo_wait = axis_stepper_diagnostics_fifo_wait,
    .write_begin = axis_stepper_diagnostics_write_begin,
    .write_end = axis_stepper_diagnostics_write_end,
    .write_error = axis_stepper_diagnostics_write_error,
    .words_written = axis_stepper_diagnostics_words_written,
    .report = axis_stepper_diagnostics_report
};

const axis_stepper_diagnostics_api_t axis_stepper_diagnostics_dummy_api = {
    .enabled = 0,
    .create = dummy_create,
    .destroy = dummy_destroy,
    .now_ns = dummy_now_ns,
    .queue_depth = dummy_queue_depth,
    .producer_wait_begin = dummy_producer_wait_begin,
    .producer_wait_end = dummy_producer_wait_end,
    .fifo_wait = dummy_fifo_wait,
    .write_begin = dummy_write_begin,
    .write_end = dummy_write_end,
    .write_error = dummy_write_error,
    .words_written = dummy_words_written,
    .report = dummy_report
};

static _Atomic(const axis_stepper_diagnostics_api_t *) selected_api = &axis_stepper_diagnostics_default_api;

void axis_stepper_diagnostics_set_api(const axis_stepper_diagnostics_api_t *api)
{
    atomic_store_explicit(&selected_api,
                          api ? api : &axis_stepper_diagnostics_default_api,
                          memory_order_release);
}

const axis_stepper_diagnostics_api_t *axis_stepper_diagnostics_get_api(void)
{
    return atomic_load_explicit(&selected_api, memory_order_acquire);
}