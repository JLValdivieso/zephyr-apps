#ifndef VIRTIO_SHM_ALLOC_H
#define VIRTIO_SHM_ALLOC_H

#include <stddef.h>
#include <stdint.h>

/**
 * Inicializa el allocator sobre la región de memoria compartida.
 * Se llama una vez al arranque, antes de que se inicialice el driver virtio.
 */
void virtio_shm_init(uintptr_t base, size_t size);

/**
 * Asigna 'size' bytes alineados a 'align' dentro de la región compartida.
 * Retorna NULL si no hay espacio.
 */
void *virtio_shm_alloc(size_t size, size_t align);

/**
 * No-op. El bump allocator no soporta free individual.
 */
void virtio_shm_free(void *ptr);

#endif