/*
 * NovexOS - pmm.c
 * Physical Memory Manager — bitmap-based page allocator.
 *
 * Uses a bitmap where each bit represents one 4KiB page.
 * Bit = 1 means used, bit = 0 means free.
 */

#include "pmm.h"
#include "string.h"

/* Maximum supported physical memory: 128 MiB = 32768 pages */
#define MAX_PAGES 32768
#define BITMAP_SIZE (MAX_PAGES / 32)

/* Bitmap: 1 = used, 0 = free */
static uint32_t bitmap[BITMAP_SIZE];
static uint32_t total_pages_count;
static uint32_t used_pages_count;
static uint32_t total_mem_kb;

/* ------- Bitmap helpers ------- */
static inline void bitmap_set(uint32_t page) {
  bitmap[page / 32] |= (1u << (page % 32));
}

static inline void bitmap_clear(uint32_t page) {
  bitmap[page / 32] &= ~(1u << (page % 32));
}

static inline int bitmap_test(uint32_t page) {
  return (bitmap[page / 32] >> (page % 32)) & 1;
}

/* ------- PMM Init ------- */
void pmm_init(struct multiboot_info *mbi) {
  /* Start by marking ALL pages as used */
  memset(bitmap, 0xFF, sizeof(bitmap));
  total_pages_count = 0;
  used_pages_count = 0;

  /* Use Multiboot mem_upper if no mmap available */
  if (mbi && (mbi->flags & (1 << 0))) {
    total_mem_kb = mbi->mem_upper + 1024; /* mem_upper is above 1MiB */
  } else {
    total_mem_kb = 16 * 1024; /* Default: assume 16 MiB */
  }

  /* Calculate total pages (cap at MAX_PAGES) */
  total_pages_count = total_mem_kb / (PAGE_SIZE / 1024);
  if (total_pages_count > MAX_PAGES) {
    total_pages_count = MAX_PAGES;
  }

  /*
   * If Multiboot provides a memory map, use it.
   * Otherwise, heuristically mark memory above 1MiB as free.
   */
  if (mbi && (mbi->flags & (1 << 6))) {
    /* Use Multiboot memory map */
    uint32_t mmap_end = mbi->mmap_addr + mbi->mmap_length;
    uint32_t addr = mbi->mmap_addr;

    while (addr < mmap_end) {
      struct multiboot_mmap_entry *entry =
          (struct multiboot_mmap_entry *)(uintptr_t)addr;

      if (entry->type == 1 && entry->addr_high == 0) {
        /* Available memory region */
        uint32_t region_start = entry->addr_low;
        uint32_t region_len = entry->len_low;
        uint32_t region_end = region_start + region_len;

        /* Align start up to page boundary */
        if (region_start & (PAGE_SIZE - 1)) {
          region_start = (region_start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        }

        /* Mark pages as free */
        for (uint32_t page_addr = region_start;
             page_addr + PAGE_SIZE <= region_end; page_addr += PAGE_SIZE) {
          uint32_t page = page_addr / PAGE_SIZE;
          if (page < total_pages_count && page >= 256) {
            /* Skip first 1MiB (256 pages) — reserved for BIOS/kernel */
            bitmap_clear(page);
          }
        }
      }

      addr += entry->size + sizeof(uint32_t);
    }
  } else {
    /* No mmap: mark pages above 2MiB as free (kernel is at 1MiB) */
    for (uint32_t page = 512; page < total_pages_count; page++) {
      bitmap_clear(page);
    }
  }

  /* Count used pages */
  used_pages_count = 0;
  for (uint32_t i = 0; i < total_pages_count; i++) {
    if (bitmap_test(i)) {
      used_pages_count++;
    }
  }
}

/* ------- Allocate a page ------- */
uint32_t pmm_alloc_page(void) {
  for (uint32_t i = 0; i < BITMAP_SIZE; i++) {
    if (bitmap[i] != 0xFFFFFFFF) {
      /* There's a free bit in this dword */
      for (int bit = 0; bit < 32; bit++) {
        if (!(bitmap[i] & (1u << bit))) {
          uint32_t page = i * 32 + (uint32_t)bit;
          if (page >= total_pages_count)
            return 0;
          bitmap_set(page);
          used_pages_count++;
          return page * PAGE_SIZE;
        }
      }
    }
  }
  return 0; /* Out of memory */
}

/* ------- Free a page ------- */
void pmm_free_page(uint32_t addr) {
  uint32_t page = addr / PAGE_SIZE;
  if (page < total_pages_count && bitmap_test(page)) {
    bitmap_clear(page);
    used_pages_count--;
  }
}

/* ------- Stats ------- */
uint32_t pmm_total_pages(void) { return total_pages_count; }
uint32_t pmm_used_pages(void) { return used_pages_count; }
uint32_t pmm_free_pages(void) { return total_pages_count - used_pages_count; }
uint32_t pmm_total_memory_kb(void) { return total_mem_kb; }
