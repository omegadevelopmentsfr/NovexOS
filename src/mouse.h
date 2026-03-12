/*
 * NovexOS - mouse.h
 * PS/2 Mouse driver.
 */

#ifndef MOUSE_H
#define MOUSE_H

#include "types.h"

/* Initialize the PS/2 mouse (enable aux port, IRQ12) */
void mouse_init(void);

/* Set screen bounds for clamping */
void mouse_set_bounds(int32_t max_x, int32_t max_y);

/* Get current mouse state */
int32_t mouse_get_x(void);
int32_t mouse_get_y(void);
uint8_t mouse_get_buttons(void);

#endif /* MOUSE_H */
