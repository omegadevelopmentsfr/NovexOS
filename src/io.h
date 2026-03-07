/*
 * OmegaOS - io.h
 * Inline assembly wrappers for x86 I/O port access.
 * Zero external dependencies.
 */

#ifndef IO_H
#define IO_H

#include "types.h"

/* Read a byte from an I/O port */
static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

/* Write a byte to an I/O port */
static inline void outb(uint16_t port, uint8_t val) {
  __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* Small delay — writing to port 0x80 takes ~1 µs on most hardware */
static inline void io_wait(void) { outb(0x80, 0); }

/* Reboot the system via PS/2 controller reset command */
static inline void reboot(void) {
  uint8_t good = 0x02;
  while (good & 0x02)
    good = inb(0x64);
  outb(0x64, 0xFE);
}

/* Write a 16-bit word to an I/O port */
static inline void outw(uint16_t port, uint16_t val) {
  __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

#endif /* IO_H */
