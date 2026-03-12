/*
 * NovexOS - io.h
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

/* Read a 16-bit word from an I/O port */
static inline uint16_t inw(uint16_t port) {
  uint16_t ret;
  __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

/* Write a 16-bit word to an I/O port */
static inline void outw(uint16_t port, uint16_t val) {
  __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

/* Read a 32-bit dword from an I/O port */
static inline uint32_t inl(uint16_t port) {
  uint32_t ret;
  __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

/* Write a 32-bit dword to an I/O port */
static inline void outl(uint16_t port, uint32_t val) {
  __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

/* Print a string to COM1 (0x3F8) for debugging */
static inline void serial_print(const char *s) {
  while (*s) {
    outb(0x3F8, *s);
    s++;
  }
}

/* Print a 32-bit hex value to COM1 */
static inline void serial_print_hex(uint32_t val) {
  serial_print("0x");
  for (int i = 28; i >= 0; i -= 4) {
    uint8_t nibble = (val >> i) & 0xF;
    if (nibble < 10)
      outb(0x3F8, '0' + nibble);
    else
      outb(0x3F8, 'A' + (nibble - 10));
  }
}

#endif /* IO_H */
