/*
 * OmegaOS - idt.c
 * 64-bit IDT Implementation.
 */

#include "idt.h"
#include "gdt.h"
#include "string.h"

static struct idt_entry idt[256];
static struct idt_ptr idtp;

extern void isr_handler(void);
/* Note: We need assembly stubs for 256 ISRs.
   For 64-bit transition proof-of-concept, we'll implement a few common ones or
   a generic handler. Usually we have isr0, isr1... in assembly.
*/

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
  idt[num].base_low = (base & 0xFFFF);
  idt[num].base_mid = (base >> 16) & 0xFFFF;
  idt[num].base_high = (base >> 32) & 0xFFFFFFFF;

  idt[num].sel = sel;
  idt[num].ist = 0;
  idt[num].flags = flags;
  idt[num].reserved = 0;
}

/* This will be called by kernel_main */
void idt_init(void) {
  idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
  idtp.base = (uint64_t)&idt;

  memset(&idt, 0, sizeof(struct idt_entry) * 256);

  /* Re-install ISRs (we need to update isr.c/isr.s first) */
  /* For now, just load the IDT */
  __asm__ volatile("lidt %0" : : "m"(idtp));
}
