#ifndef ZYNQ_OBJECT_POOL_H
#define ZYNQ_OBJECT_POOL_H

#include <stddef.h>

typedef struct object_pool_s *object_pool_t;

object_pool_t object_pool_create(size_t object_size, size_t cache_bytes, size_t chunk_bytes);
void object_pool_destroy(object_pool_t pool);
int object_pool_acquire(object_pool_t pool, void **object, unsigned timeout_ms);
int object_pool_publish(object_pool_t pool, void *object);
int object_pool_take(object_pool_t pool, void **object);
int object_pool_release(object_pool_t pool, void *object);
void object_pool_stop(object_pool_t pool);
void object_pool_get_stats(object_pool_t pool, size_t *used, size_t *ready, size_t *capacity);
size_t object_pool_memory_bytes(object_pool_t pool);

#endif