#include "virtio_shm_alloc.h"

static uintptr_t shm_base;
static uintptr_t shm_offset;
static size_t shm_total;

void virtio_shm_init(uintptr_t base, size_t size)
{
    shm_base = base;
    shm_offset = 0;
    shm_total = size;
}

void *virtio_shm_alloc(size_t size, size_t align)
{
    uintptr_t current = shm_base + shm_offset;
    uintptr_t aligned = (current + align - 1) & ~(align - 1);
    size_t padding = aligned - current;

    if (shm_offset + padding + size > shm_total) {
        return NULL;
    }

    shm_offset += padding + size;
    return (void *)aligned;
}

void virtio_shm_free(void *ptr)
{
    (void)ptr;
}