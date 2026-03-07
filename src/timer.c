/*
 * OmegaOS - timer.c
 * PIT (Programmable Interval Timer) driver — IRQ0.
 * Programs channel 0 in rate generator mode.
 */

#include "timer.h"
#include "io.h"
#include "isr.h"

/* PIT base frequency: 1.193182 MHz */
#define PIT_FREQUENCY 1193182

/* PIT I/O ports */
#define PIT_CHANNEL0 0x40
#define PIT_COMMAND 0x43

/* Timer state */
static volatile uint32_t system_ticks = 0;
static uint32_t timer_freq = 0;

/* Timer interrupt handler (callback) */
static void timer_callback(registers_t *regs) {
  (void)regs;
  system_ticks++;
}

void timer_init(uint32_t frequency) {
  timer_freq = frequency;

  /* Calculate divisor */
  uint32_t divisor = PIT_FREQUENCY / frequency;

  /* Send command: channel 0, lobyte/hibyte, rate generator */
  outb(PIT_COMMAND, 0x36);

  /* Send divisor */
  outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));        /* Low byte */
  outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF)); /* High byte */

  /* Register IRQ0 (IDT 32) handler */
  register_interrupt_handler(32, timer_callback);
}

uint32_t timer_get_ticks(void) { return system_ticks; }

uint32_t timer_get_uptime_seconds(void) {
  if (timer_freq == 0)
    return 0;
  return system_ticks / timer_freq;
}

void timer_sleep(uint32_t ticks) {
  uint32_t target = system_ticks + ticks;
  while (system_ticks < target) {
    __asm__ volatile("hlt");
  }
}
