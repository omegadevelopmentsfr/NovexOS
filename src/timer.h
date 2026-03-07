/*
 * OmegaOS - timer.h
 * Programmable Interval Timer (PIT) driver.
 */

#ifndef TIMER_H
#define TIMER_H

#include "types.h"

/* Initialize PIT at the given frequency (Hz) */
void timer_init(uint32_t frequency);

/* Get system ticks since boot */
uint32_t timer_get_ticks(void);

/* Get uptime in seconds */
uint32_t timer_get_uptime_seconds(void);

/* Wait for a number of ticks */
void timer_sleep(uint32_t ticks);

#endif /* TIMER_H */
