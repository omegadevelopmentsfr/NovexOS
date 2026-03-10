/*
 * NovexOS - heap.c
 * Simple linked-list first-fit heap allocator.
 *
 * The heap is a contiguous region obtained from the PMM.
 * Each block has a header with its size and a free flag.
 */

#include "heap.h"
#include "pmm.h"
#include "string.h"

/* Block header */
struct heap_block {
  uint32_t size;           /* Size of data area (excludes header) */
  int free;                /* 1 = free, 0 = used */
  struct heap_block *next; /* Next block in the list */
};

#define HEADER_SIZE sizeof(struct heap_block)
#define HEAP_PAGES 16 /* 64 KiB initial heap */

static struct heap_block *heap_start = NULL;
static uint32_t heap_total_size = 0;
static uint32_t heap_used_bytes = 0;

void heap_init(void) {
  /* Allocate pages for the heap from PMM */
  uint32_t first_page = pmm_alloc_page();
  if (!first_page)
    return;

  /* Allocate contiguous pages (best effort) */
  heap_total_size = PAGE_SIZE;
  for (int i = 1; i < HEAP_PAGES; i++) {
    uint32_t page = pmm_alloc_page();
    if (!page)
      break;
    /* We hope pages are contiguous; for simplicity, we use the first block */
    heap_total_size += PAGE_SIZE;
  }

  /* Initialize the heap as one big free block */
  heap_start = (struct heap_block *)(uintptr_t)first_page;
  heap_start->size = heap_total_size - HEADER_SIZE;
  heap_start->free = 1;
  heap_start->next = NULL;
  heap_used_bytes = 0;
}

void *kmalloc(size_t size) {
  if (!heap_start || size == 0)
    return NULL;

  /* Align to 4 bytes */
  size = (size + 3) & ~((size_t)3);

  struct heap_block *current = heap_start;

  /* First-fit search */
  while (current) {
    if (current->free && current->size >= size) {
      /* Split block if remainder is large enough */
      if (current->size >= size + HEADER_SIZE + 16) {
        struct heap_block *new_block =
            (struct heap_block *)((uint8_t *)current + HEADER_SIZE + size);
        new_block->size = current->size - size - HEADER_SIZE;
        new_block->free = 1;
        new_block->next = current->next;
        current->next = new_block;
        current->size = size;
      }

      current->free = 0;
      heap_used_bytes += current->size;
      return (void *)((uint8_t *)current + HEADER_SIZE);
    }
    current = current->next;
  }

  return NULL; /* No suitable block found */
}

void kfree(void *ptr) {
  if (!ptr)
    return;

  struct heap_block *block =
      (struct heap_block *)((uint8_t *)ptr - HEADER_SIZE);
  block->free = 1;
  heap_used_bytes -= block->size;

  /* Coalesce adjacent free blocks */
  struct heap_block *current = heap_start;
  while (current) {
    if (current->free && current->next && current->next->free) {
      current->size += HEADER_SIZE + current->next->size;
      current->next = current->next->next;
      continue; /* Check again in case of multiple adjacent free blocks */
    }
    current = current->next;
  }
}

uint32_t heap_used(void) { return heap_used_bytes; }
uint32_t heap_free(void) {
  return heap_total_size - heap_used_bytes - HEADER_SIZE;
}
