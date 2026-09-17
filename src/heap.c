/*
 * NovexOS - heap.c
 * Simple linked-list first-fit heap allocator.
 *
 * The heap is a contiguous region obtained from the PMM.
 * Each block has a header with its size and a free flag.
 */

#include "heap.h"
#include "io.h"
#include "pmm.h"
#include "string.h"

/* Block header */
struct heap_block {
  uint32_t size;           /* Size of data area (excludes header) */
  int free;                /* 1 = free, 0 = used */
  struct heap_block *next; /* Next block in the list */
};

#define HEADER_SIZE sizeof(struct heap_block)
#define HEAP_PAGES 256 /* 1 MiB heap (for NovexDE GUI) */

/* Forward declaration: not relying on heap.h having this prototype,
 * since it's temporary diagnostic instrumentation. */
void heap_debug_dump(const char *label);

static struct heap_block *heap_start = NULL;
static uint32_t heap_total_size = 0;
static uint32_t heap_used_bytes = 0;

void heap_init(void) {
  /* Allocate pages for the heap from PMM */
  uint32_t first_page = pmm_alloc_page();
  if (!first_page)
    return;

  /* Allocate contiguous pages. Unlike before, we now VERIFY contiguity
   * instead of assuming it: pmm_alloc_page() gives no guarantee that
   * consecutive calls return adjacent physical pages (e.g. if a page
   * in between is already reserved/used). Treating non-contiguous
   * pages as one flat heap region corrupts block headers -> garbage
   * `next` pointers -> #GP fault when walking the free list (as seen
   * when kfree() dereferences a corrupted next pointer). */
  heap_total_size = PAGE_SIZE;
  uint32_t expected_next = first_page + PAGE_SIZE;
  for (int i = 1; i < HEAP_PAGES; i++) {
    uint32_t page = pmm_alloc_page();
    if (!page)
      break;
    if (page != expected_next) {
      /* Non-contiguous page: give it back and stop growing the heap
       * here rather than silently corrupting memory later. */
      serial_print("[HEAP] init: non-contiguous page at ");
      serial_print_hex(page);
      serial_print(" (expected ");
      serial_print_hex(expected_next);
      serial_print("), stopping heap growth here\r\n");
      pmm_free_page(page);
      break;
    }
    heap_total_size += PAGE_SIZE;
    expected_next += PAGE_SIZE;
  }

  serial_print("[HEAP] init: heap_total_size=");
  serial_print_hex(heap_total_size);
  serial_print(" bytes (");
  serial_print_hex(heap_total_size / PAGE_SIZE);
  serial_print(" pages) starting at ");
  serial_print_hex(first_page);
  serial_print("\r\n");

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

  serial_print("[HEAP] kfree: enter ptr=");
  serial_print_hex((uint32_t)(uintptr_t)ptr);
  serial_print("\r\n");

  struct heap_block *block =
      (struct heap_block *)((uint8_t *)ptr - HEADER_SIZE);
  serial_print("[HEAP] kfree: block=");
  serial_print_hex((uint32_t)(uintptr_t)block);
  serial_print(" size=");
  serial_print_hex(block->size);
  serial_print(" free_flag_before=");
  serial_print_hex((uint32_t)block->free);
  serial_print("\r\n");

  block->free = 1;
  heap_used_bytes -= block->size;

  serial_print("[HEAP] kfree: starting coalesce\r\n");

  heap_debug_dump("kfree pre-coalesce");

  /* Coalesce adjacent free blocks.
   * SAFETY: capped iteration count to detect a corrupted (cyclic)
   * free list instead of hanging forever. If the cap is hit, the
   * heap is corrupted somewhere upstream of this call. */
  struct heap_block *current = heap_start;
  uint32_t guard = 0;
  const uint32_t GUARD_MAX = 200000;
  while (current) {
    guard++;
    if (guard > GUARD_MAX) {
      serial_print("[HEAP] kfree: !!! COALESCE LOOP GUARD TRIPPED - heap "
                   "free-list is corrupted (cycle?) current=");
      serial_print_hex((uint32_t)(uintptr_t)current);
      serial_print("\r\n");
      return;
    }
    if (current->free && current->next && current->next->free) {
      current->size += HEADER_SIZE + current->next->size;
      current->next = current->next->next;
      continue; /* Check again in case of multiple adjacent free blocks */
    }
    current = current->next;
  }

  serial_print("[HEAP] kfree: coalesce done, iterations=");
  serial_print_hex(guard);
  serial_print("\r\n");
}

uint32_t heap_used(void) { return heap_used_bytes; }
uint32_t heap_free(void) {
  return heap_total_size - heap_used_bytes - HEADER_SIZE;
}

void heap_debug_dump(const char *label) {
  serial_print("[HEAP] --- dump: ");
  serial_print(label);
  serial_print(" ---\r\n");

  uint8_t *heap_lo = (uint8_t *)heap_start;
  uint8_t *heap_hi = heap_lo + heap_total_size;

  struct heap_block *current = heap_start;
  uint32_t guard = 0;
  const uint32_t GUARD_MAX = 4096; /* plenty for a sane list */
  while (current && guard < GUARD_MAX) {
    guard++;

    serial_print("[HEAP]   #");
    serial_print_hex(guard);
    serial_print(" addr=");
    serial_print_hex((uint32_t)(uintptr_t)current);

    /* Sanity-check the pointer itself before touching its fields */
    if ((uint8_t *)current < heap_lo || (uint8_t *)current >= heap_hi) {
      serial_print(" <<< OUT OF HEAP BOUNDS! (heap=[");
      serial_print_hex((uint32_t)(uintptr_t)heap_lo);
      serial_print(",");
      serial_print_hex((uint32_t)(uintptr_t)heap_hi);
      serial_print(") - stopping walk\r\n");
      return;
    }

    serial_print(" size=");
    serial_print_hex(current->size);
    serial_print(" free=");
    serial_print_hex((uint32_t)current->free);
    serial_print(" next=");
    serial_print_hex((uint32_t)(uintptr_t)current->next);
    serial_print("\r\n");

    current = current->next;
  }

  if (guard >= GUARD_MAX) {
    serial_print("[HEAP]   <<< guard hit, list likely cyclic\r\n");
  }

  serial_print("[HEAP] --- end dump ---\r\n");
}