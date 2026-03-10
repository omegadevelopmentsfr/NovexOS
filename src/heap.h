/*
 * NovexOS - heap.h
 * Dynamic memory allocator — kmalloc / kfree.
 */

#ifndef HEAP_H
#define HEAP_H

#include "types.h"

/* Initialize the kernel heap */
void heap_init(void);

/* Allocate 'size' bytes on the heap */
void *kmalloc(size_t size);

/* Free a previously allocated block */
void kfree(void *ptr);

/* Heap statistics */
uint32_t heap_used(void);
uint32_t heap_free(void);

#endif /* HEAP_H */
