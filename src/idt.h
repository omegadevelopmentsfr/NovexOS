/*
 * OmegaOS - idt.h
 * 64-bit Interrupt Descriptor Table.
 */

#ifndef IDT_H
#define IDT_H

#include "types.h"

/* IDT Entry (16 bytes) */
struct idt_entry {
  uint16_t base_low;
  uint16_t sel;
  uint8_t ist;   /* Interrupt Stack Table offset */
  uint8_t flags; /* Type and Attributes */
  uint16_t base_mid;
  uint32_t base_high;
  uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
  uint16_t limit;
  uint64_t base;
} __attribute__((packed));

void idt_init(void);

#endif /* IDT_H */
