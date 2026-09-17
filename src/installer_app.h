/*
 * NovexOS - installer_app.h
 * Graphical installer application for NovexDE.
 */

#ifndef INSTALLER_APP_H
#define INSTALLER_APP_H

#include "gui.h"
#include "types.h"

/* Initialize the installer app (call once after desktop_init) */
void installer_app_init(void);

/* Render the installer window (called from desktop_loop_iteration) */
void installer_app_render(void);

/* Handle a mouse click at (mx,my); returns 1 if consumed */
int installer_app_click(int32_t mx, int32_t my);

/* Handle a key press; returns 1 if consumed */
int installer_app_key(char c);

/* Get a pointer to the window for visibility / drag management */
gui_window_t *installer_app_get_window(void);

#endif /* INSTALLER_APP_H */
