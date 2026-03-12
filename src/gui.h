/*
 * NovexOS - gui.h
 * Simple GUI widget structures and helpers for NovexDE.
 */

#ifndef GUI_H
#define GUI_H

#include "types.h"

/* Rectangle */
typedef struct {
  int32_t x, y, w, h;
} gui_rect_t;

/* Window structure */
typedef struct {
  gui_rect_t bounds;     /* Position and size */
  const char *title;     /* Window title */
  int visible;           /* Is the window shown? */
  int dragging;          /* Is the user dragging the title bar? */
  int32_t drag_offset_x; /* Offset from mouse to window origin */
  int32_t drag_offset_y;
} gui_window_t;

/* Color constants — NovexDE theme */
#define COLOR_BG_TOP 0xFF1A1A2E    /* Deep navy (wallpaper gradient top) */
#define COLOR_BG_BOTTOM 0xFF0F3460 /* Deep blue (wallpaper gradient bottom) */
#define COLOR_TASKBAR 0xE0202040   /* Dark semi-transparent taskbar */
#define COLOR_TASKBAR_TEXT 0xFFE0E0E0 /* Light gray text */
#define COLOR_ACCENT 0xFF6C63FF       /* Purple/violet accent (NovexOS brand) */
#define COLOR_ACCENT_HOVER 0xFF8A84FF /* Lighter accent on hover */
#define COLOR_WIN_BG 0xFF1E1E2E       /* Window background */
#define COLOR_WIN_TITLE 0xFF2D2D44    /* Window title bar */
#define COLOR_WIN_BORDER 0xFF3A3A5C   /* Window border */
#define COLOR_WIN_CLOSE 0xFFE06060    /* Close button (red) */
#define COLOR_TEXT_WHITE 0xFFFFFFFF
#define COLOR_TEXT_GREEN 0xFF50FA7B  /* Terminal green */
#define COLOR_TEXT_CYAN 0xFF8BE9FD   /* Terminal cyan */
#define COLOR_TEXT_YELLOW 0xFFF1FA8C /* Terminal yellow */
#define COLOR_TEXT_GRAY 0xFF888888
#define COLOR_TEXT_BLACK 0xFF000000
#define COLOR_CURSOR 0xFFFFFFFF /* Mouse cursor color */
#define COLOR_TRANSPARENT 0x00000000

/* Taskbar dimensions */
#define TASKBAR_HEIGHT 40
#define TITLEBAR_HEIGHT 28

/* Check if point is inside rectangle */
int gui_point_in_rect(int32_t px, int32_t py, const gui_rect_t *r);

/* Initialize a window */
void gui_window_init(gui_window_t *win, int32_t x, int32_t y, int32_t w,
                     int32_t h, const char *title);

/* Render a window frame (title bar + border + background) */
void gui_window_render(const gui_window_t *win);

/* Render a button-like widget */
void gui_draw_button(int32_t x, int32_t y, int32_t w, int32_t h,
                     const char *label, uint32_t bg, uint32_t fg);

#endif /* GUI_H */
