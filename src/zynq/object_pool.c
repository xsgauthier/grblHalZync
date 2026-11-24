#include "object_pool.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

struct object_pool_s {
    void **chunks;
    void **ready_queue;
    void **free_objects;
    size_t object_stride;
    size_t capacity;
    size_t chunk_count;
    size_t ready_head;
    size_t ready_tail;
    size_t ready_count;
    size_t free_count;
    size_t memory_bytes;
    pthread_mutex_t mutex;
    pthread_cond_t free_ready;
    pthread_cond_t data_ready;
    pthread_cond_t ready_space;
    int stopping;
};

static void release_storage(object_pool_t pool)
{
    if (pool->chunks) {
        for (size_t i = 0; i < pool->chunk_count; i++)
            free(pool->chunks[i]);
    }

    free(pool->chunks);
    free(pool->ready_queue);
    free(pool->free_objects);
}

object_pool_t object_pool_create(size_t object_size, size_t cache_bytes, size_t chunk_bytes)
{
    const size_t alignment = _Alignof(max_align_t);
    size_t objects_per_chunk;
    size_t object_index = 0;
    size_t mutex_initialized = 0;
    size_t free_ready_initialized = 0;
    size_t data_ready_initialized = 0;
    size_t ready_space_initialized = 0;
    object_pool_t pool;
    int error = ENOMEM;

    if (object_size == 0 || cache_bytes == 0 || chunk_bytes == 0 ||
        object_size > SIZE_MAX - (alignment - 1))
        return NULL;

    pool = calloc(1, sizeof(*pool));
    if (!pool)
        return NULL;

    pool->object_stride = (object_size + alignment - 1) / alignment * alignment;
    if (pool->object_stride > SIZE_MAX - 2 * sizeof(void *))
        goto fail;

    pool->capacity = cache_bytes / (pool->object_stride + 2 * sizeof(void *));
    if (pool->capacity == 0)
        goto fail;

    objects_per_chunk = chunk_bytes / pool->object_stride;
    if (objects_per_chunk == 0)
        objects_per_chunk = 1;
    pool->chunk_count = (pool->capacity + objects_per_chunk - 1) / objects_per_chunk;

    if (pool->capacity > SIZE_MAX / sizeof(void *) ||
        pool->chunk_count > SIZE_MAX / sizeof(void *) ||
        pool->capacity > SIZE_MAX / pool->object_stride)
        goto fail;

    pool->chunks = calloc(pool->chunk_count, sizeof(*pool->chunks));
    pool->ready_queue = calloc(pool->capacity, sizeof(*pool->ready_queue));
    pool->free_objects = malloc(pool->capacity * sizeof(*pool->free_objects));
    if (!pool->chunks || !pool->ready_queue || !pool->free_objects)
        goto fail;

    for (size_t i = 0; i < pool->chunk_count; i++) {
        size_t chunk_objects = pool->capacity - object_index;
        if (chunk_objects > objects_per_chunk)
            chunk_objects = objects_per_chunk;

        pool->chunks[i] = calloc(chunk_objects, pool->object_stride);
        if (!pool->chunks[i])
            goto fail;

        for (size_t j = 0; j < chunk_objects; j++) {
            pool->free_objects[object_index++] = (unsigned char *)pool->chunks[i]
                + j * pool->object_stride;
        }
    }

    pool->free_count = pool->capacity;
    pool->memory_bytes = sizeof(*pool)
        + pool->capacity * (pool->object_stride + 2 * sizeof(void *))
        + pool->chunk_count * sizeof(*pool->chunks);

    error = pthread_mutex_init(&pool->mutex, NULL);
    if (error)
        goto fail;
    mutex_initialized = 1;

    error = pthread_cond_init(&pool->free_ready, NULL);
    if (error)
        goto fail;
    free_ready_initialized = 1;

    error = pthread_cond_init(&pool->data_ready, NULL);
    if (error)
        goto fail;
    data_ready_initialized = 1;

    error = pthread_cond_init(&pool->ready_space, NULL);
    if (error)
        goto fail;
    ready_space_initialized = 1;

    return pool;

fail:
    if (ready_space_initialized)
        pthread_cond_destroy(&pool->ready_space);
    if (data_ready_initialized)
        pthread_cond_destroy(&pool->data_ready);
    if (free_ready_initialized)
        pthread_cond_destroy(&pool->free_ready);
    if (mutex_initialized)
        pthread_mutex_destroy(&pool->mutex);
    release_storage(pool);
    free(pool);
    errno = error ? error : ENOMEM;
    return NULL;
}

void object_pool_destroy(object_pool_t pool)
{
    if (!pool)
        return;

    pthread_cond_destroy(&pool->ready_space);
    pthread_cond_destroy(&pool->data_ready);
    pthread_cond_destroy(&pool->free_ready);
    pthread_mutex_destroy(&pool->mutex);
    release_storage(pool);
    free(pool);
}

int object_pool_acquire(object_pool_t pool, void **object, unsigned timeout_ms)
{
    struct timespec deadline;

    if (!pool || !object) {
        errno = EINVAL;
        return -1;
    }

    *object = NULL;
    if (timeout_ms && clock_gettime(CLOCK_REALTIME, &deadline) != 0)
        return -1;
    if (timeout_ms) {
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    pthread_mutex_lock(&pool->mutex);
    while (pool->free_count == 0 && !pool->stopping) {
        if (!timeout_ms) {
            pthread_mutex_unlock(&pool->mutex);
            return 0;
        }

        int error = pthread_cond_timedwait(&pool->free_ready, &pool->mutex, &deadline);
        if (error == ETIMEDOUT && pool->free_count == 0) {
            pthread_mutex_unlock(&pool->mutex);
            return 0;
        }
        if (error && error != ETIMEDOUT) {
            pthread_mutex_unlock(&pool->mutex);
            errno = error;
            return -1;
        }
    }

    if (pool->stopping) {
        pthread_mutex_unlock(&pool->mutex);
        return -1;
    }

    *object = pool->free_objects[--pool->free_count];
    pthread_mutex_unlock(&pool->mutex);
    return 1;
}

int object_pool_publish(object_pool_t pool, void *object)
{
    if (!pool || !object) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&pool->mutex);
    while (pool->ready_count == pool->capacity && !pool->stopping)
        pthread_cond_wait(&pool->ready_space, &pool->mutex);

    if (pool->stopping) {
        pthread_mutex_unlock(&pool->mutex);
        errno = ECANCELED;
        return -1;
    }

    pool->ready_queue[pool->ready_tail] = object;
    pool->ready_tail = (pool->ready_tail + 1) % pool->capacity;
    pool->ready_count++;
    pthread_cond_signal(&pool->data_ready);
    pthread_mutex_unlock(&pool->mutex);
    return 0;
}

int object_pool_take(object_pool_t pool, void **object)
{
    if (!pool || !object) {
        errno = EINVAL;
        return -1;
    }

    *object = NULL;
    pthread_mutex_lock(&pool->mutex);
    while (pool->ready_count == 0 && !pool->stopping) {
        int error = pthread_cond_wait(&pool->data_ready, &pool->mutex);
        if (error) {
            pthread_mutex_unlock(&pool->mutex);
            errno = error;
            return -1;
        }
    }

    if (pool->ready_count == 0) {
        pthread_mutex_unlock(&pool->mutex);
        return 0;
    }

    *object = pool->ready_queue[pool->ready_head];
    pool->ready_head = (pool->ready_head + 1) % pool->capacity;
    pool->ready_count--;
    pthread_cond_signal(&pool->ready_space);
    pthread_mutex_unlock(&pool->mutex);
    return 1;
}

int object_pool_release(object_pool_t pool, void *object)
{
    if (!pool || !object) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&pool->mutex);
    if (pool->free_count == pool->capacity) {
        pthread_mutex_unlock(&pool->mutex);
        errno = EOVERFLOW;
        return -1;
    }

    pool->free_objects[pool->free_count++] = object;
    pthread_cond_signal(&pool->free_ready);
    pthread_mutex_unlock(&pool->mutex);
    return 0;
}

void object_pool_stop(object_pool_t pool)
{
    if (!pool)
        return;

    pthread_mutex_lock(&pool->mutex);
    pool->stopping = 1;
    pthread_cond_broadcast(&pool->free_ready);
    pthread_cond_broadcast(&pool->data_ready);
    pthread_cond_broadcast(&pool->ready_space);
    pthread_mutex_unlock(&pool->mutex);
}

void object_pool_get_stats(object_pool_t pool, size_t *used, size_t *ready, size_t *capacity)
{
    pthread_mutex_lock(&pool->mutex);
    if (used)
        *used = pool->capacity - pool->free_count;
    if (ready)
        *ready = pool->ready_count;
    if (capacity)
        *capacity = pool->capacity;
    pthread_mutex_unlock(&pool->mutex);
}

size_t object_pool_memory_bytes(object_pool_t pool)
{
    return pool ? pool->memory_bytes : 0;
}