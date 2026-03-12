/*
 * NovexOS - vbe.h
 * VESA VBE framebuffer driver via Multiboot info.
 */

#ifndef VBE_H
#define VBE_H

#include "types.h"

/* Multiboot VBE mode info (subset of fields we need) */
struct vbe_mode_info {
  uint16_t attributes;
  uint8_t window_a, window_b;
  uint16_t granularity, window_size;
  uint16_t segment_a, segment_b;
  uint32_t win_func_ptr;
  uint16_t pitch;         /* Bytes per scan line */
  uint16_t width, height; /* Resolution */
  uint8_t w_char, y_char, planes, bpp, banks;
  uint8_t memory_model, bank_size, image_pages;
  uint8_t reserved0;
  /* Direct color fields */
  uint8_t red_mask, red_position;
  uint8_t green_mask, green_position;
  uint8_t blue_mask, blue_position;
  uint8_t rsv_mask, rsv_position;
  uint8_t directcolor_attributes;
  uint32_t framebuffer; /* Physical address of framebuffer */
  uint32_t off_screen_mem_off;
  uint16_t off_screen_mem_size;
} __attribute__((packed));

/* Initialize VBE from Multiboot info */
void vbe_init(void *multiboot_info);

/* Accessors */
uint32_t *vbe_get_framebuffer(void);
uint32_t vbe_get_width(void);
uint32_t vbe_get_height(void);
uint32_t vbe_get_pitch(void);
uint32_t vbe_get_bpp(void);
int vbe_is_available(void);

/* Map a physical address range into the page tables */
void vbe_map_framebuffer(uint64_t phys_addr, uint64_t size);

#endif /* VBE_H */
