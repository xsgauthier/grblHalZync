#include "logger.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOGGER_QUEUE_CAPACITY 128

typedef struct {
    FILE *stream;
    struct timespec timestamp;
    char *message;
    int message_length;
} logger_record_t;

static logger_record_t queue[LOGGER_QUEUE_CAPACITY];
static size_t queue_head;
static size_t queue_tail;
static size_t queue_count;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_ready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t queue_space = PTHREAD_COND_INITIALIZER;
static pthread_once_t logger_once = PTHREAD_ONCE_INIT;
static pthread_t logger_thread;
static int logger_started;
static int logger_stopping;

static int write_record(FILE *stream, const struct timespec *timestamp, const char *message, int message_length)
{
    struct tm local_time;
    char timestamp_text[32] = "0000-00-00 00:00:00";

    if (localtime_r(&timestamp->tv_sec, &local_time))
        strftime(timestamp_text, sizeof(timestamp_text), "%Y-%m-%d %H:%M:%S", &local_time);

    if (fprintf(stream, "[%s.%03ld] %s", timestamp_text,
                timestamp->tv_nsec / 1000000, message) < 0 || fflush(stream) == EOF)
        return -1;

    return message_length;
}

static void *logger_worker(void *arg)
{
    (void)arg;

    for (;;) {
        logger_record_t record;

        pthread_mutex_lock(&queue_mutex);
        while (queue_count == 0 && !logger_stopping)
            pthread_cond_wait(&queue_ready, &queue_mutex);

        if (queue_count == 0 && logger_stopping) {
            pthread_mutex_unlock(&queue_mutex);
            break;
        }

        record = queue[queue_head];
        queue_head = (queue_head + 1) % LOGGER_QUEUE_CAPACITY;
        queue_count--;
        pthread_cond_signal(&queue_space);
        pthread_mutex_unlock(&queue_mutex);

        write_record(record.stream, &record.timestamp, record.message, record.message_length);
        free(record.message);
    }

    return NULL;
}

static void logger_start(void)
{
    if (pthread_create(&logger_thread, NULL, logger_worker, NULL) == 0) {
        pthread_mutex_lock(&queue_mutex);
        logger_started = 1;
        pthread_mutex_unlock(&queue_mutex);
        atexit(logger_shutdown);
    }
}

int logger_fprintf(FILE *stream, const char *format, ...)
{
    struct timespec timestamp;
    va_list args, measure_args;
    char *message;
    int message_length;

    if (!stream || !format) {
        errno = EINVAL;
        return -1;
    }

    if (clock_gettime(CLOCK_REALTIME, &timestamp) != 0)
        return -1;

    va_start(args, format);
    va_copy(measure_args, args);
    message_length = vsnprintf(NULL, 0, format, measure_args);
    va_end(measure_args);
    if (message_length < 0) {
        va_end(args);
        return -1;
    }

    message = malloc((size_t)message_length + 1);
    if (!message) {
        va_end(args);
        return -1;
    }

    vsnprintf(message, (size_t)message_length + 1, format, args);
    va_end(args);

    pthread_once(&logger_once, logger_start);
    pthread_mutex_lock(&queue_mutex);

    while (queue_count == LOGGER_QUEUE_CAPACITY && !logger_stopping)
        pthread_cond_wait(&queue_space, &queue_mutex);

    if (!logger_started || logger_stopping) {
        pthread_mutex_unlock(&queue_mutex);
        int result = write_record(stream, &timestamp, message, message_length);
        free(message);
        return result;
    }

    queue[queue_tail].stream = stream;
    queue[queue_tail].timestamp = timestamp;
    queue[queue_tail].message = message;
    queue[queue_tail].message_length = message_length;
    queue_tail = (queue_tail + 1) % LOGGER_QUEUE_CAPACITY;
    queue_count++;
    pthread_cond_signal(&queue_ready);
    pthread_mutex_unlock(&queue_mutex);

    return message_length;
}

void logger_shutdown(void)
{
    pthread_mutex_lock(&queue_mutex);
    if (!logger_started || logger_stopping) {
        pthread_mutex_unlock(&queue_mutex);
        return;
    }

    logger_stopping = 1;
    pthread_cond_broadcast(&queue_ready);
    pthread_cond_broadcast(&queue_space);
    pthread_mutex_unlock(&queue_mutex);

    pthread_join(logger_thread, NULL);

    pthread_mutex_lock(&queue_mutex);
    logger_started = 0;
    pthread_mutex_unlock(&queue_mutex);
}