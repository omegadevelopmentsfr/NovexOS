/*
 * NovexOS - desktop.h
 * NovexDE — Graphical Desktop Environment.
 */

#ifndef DESKTOP_H
#define DESKTOP_H

#include "types.h"

/* Initialize the desktop environment (requires VBE and mouse to be ready) */
void desktop_init(void);

/* Main render/event loop — called from kernel halt loop when DE is active */
void desktop_run(void);

/* Event handlers (called from interrupt context) */
void desktop_handle_key(char c);

/* Check if desktop mode is active */
int desktop_is_active(void);

/* Forward a character from the main terminal to the DE terminal */
void desktop_terminal_putchar(char c);

#endif /* DESKTOP_H */
