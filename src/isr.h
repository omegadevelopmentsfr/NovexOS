/*
 * NovexOS - isr.h
 * 64-bit Interrupt Service Routines.
 */

#ifndef ISR_H
#define ISR_H

#include "types.h"

/* Registers saved by our assembly stub */
typedef struct {
  uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
  uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
  uint64_t int_no, err_code;
  uint64_t rip, cs, rflags, rsp, ss;
} registers_t;

/* Typedef for interrupt handler function */
typedef void (*isr_t)(registers_t *);

void isr_init(void);
void register_interrupt_handler(uint8_t n, isr_t handler);

#endif /* ISR_H */
