/*
 * OmegaOS - gdt.c
 * 64-bit GDT Implementation.
 */

#include "gdt.h"

static struct gdt_entry gdt[3];
static struct gdt_ptr gp;

extern void gdt_flush(uint64_t);

static void gdt_set_gate(int num, uint64_t base, uint64_t limit, uint8_t access,
                         uint8_t gran) {
  gdt[num].base_low = (base & 0xFFFF);
  gdt[num].base_middle = (base >> 16) & 0xFF;
  gdt[num].base_high = (base >> 24) & 0xFF;

  gdt[num].limit_low = (limit & 0xFFFF);
  gdt[num].granularity = (limit >> 16) & 0x0F;
  gdt[num].granularity |= (gran & 0xF0);
  gdt[num].access = access;
}

void gdt_init(void) {
  gp.limit = (sizeof(struct gdt_entry) * 3) - 1;
  gp.base = (uint64_t)&gdt;

  /* 0: Null Descriptor */
  gdt_set_gate(0, 0, 0, 0, 0);

  /* 1: Kernel Code Segment (64-bit) */
  /* Access: Present(1) | Ring0(00) | S(1) | Exec/Read(1010) = 0x9A */
  /* Flags: LongMode(1) | D(0) | G(1) = 0xAF (actually D must be 0 for 64-bit
   * code) */
  /* Long Mode bit is bit 21 of high dword, which is bit 5 of granularity byte?
   * No. */
  /* Granularity byte: G(7) | D/B(6) | L(5) | AVL(4) | Limit(3:0) */
  /* For long mode: L=1, D=0. G=1 (4KB pages). */
  /* So Flags = 0x20 (L=1, G=0?) or 0xA0 (G=1, L=1)? */
  /* Let's set Limit=0xFFFFF, G=1, L=1, D=0 -> 0xAF? No, D=0. 0x20 | 0x80? */
  /* 0xA = 1010. G=1, D=0, L=1, AVL=0. -> 0xA. Plus high limit nibble (F) ->
   * 0xAF */
  gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xAF);

  /* 2: Kernel Data Segment */
  /* Access: Present(1) | Ring0(00) | S(1) | RW(0010) = 0x92 */
  /* Flags: 0xC? or just ignored. Typically 0xCF or 0xA? */
  /* Data segment in long mode is mostly ignored but recommended to be
   * present/rw */
  gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92,
               0xCF); /* standard 32-bit data descriptors usually fine */

  /* Flush GDT */
  __asm__ volatile("lgdt %0" : : "m"(gp));

  /* Reload segments? CS is already loaded by lgdt in boot.s? */
  /* We should reload data segments */
  /* Assembler stub to reload DS, ES, FS, GS, SS */
}
