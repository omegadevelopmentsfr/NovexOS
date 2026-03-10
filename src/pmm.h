/*
 * NovexOS - pmm.h
 * Physical Memory Manager — bitmap-based page allocator.
 */

#ifndef PMM_H
#define PMM_H

#include "types.h"

#define PAGE_SIZE 4096

/* Multiboot memory map entry (from Multiboot spec) */
struct multiboot_mmap_entry {
  uint32_t size;
  uint32_t addr_low;
  uint32_t addr_high;
  uint32_t len_low;
  uint32_t len_high;
  uint32_t type; /* 1 = available, other = reserved */
} __attribute__((packed));

/* Multiboot info structure (only the fields we need) */
struct multiboot_info {
  uint32_t flags;
  uint32_t mem_lower; /* KB of lower memory (below 1MiB) */
  uint32_t mem_upper; /* KB of upper memory (above 1MiB) */
  uint32_t boot_device;
  uint32_t cmdline;
  uint32_t mods_count;
  uint32_t mods_addr;
  uint32_t syms[4];
  uint32_t mmap_length;
  uint32_t mmap_addr;
} __attribute__((packed));

/* Initialize PMM using Multiboot memory map */
void pmm_init(struct multiboot_info *mbi);

/* Allocate a single 4KiB page, returns physical address or 0 on failure */
uint32_t pmm_alloc_page(void);

/* Free a previously allocated page */
void pmm_free_page(uint32_t addr);

/* Memory statistics */
uint32_t pmm_total_pages(void);
uint32_t pmm_used_pages(void);
uint32_t pmm_free_pages(void);
uint32_t pmm_total_memory_kb(void);

#endif /* PMM_H */
