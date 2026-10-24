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

/* Color constants — NovexDE theme (palette of the NovexOS website) */
#define COLOR_BG_TOP 0xFF0A0A0F    /* cyber-black */
#define COLOR_BG_BOTTOM 0xFF0E0B22 /* black with a violet tint */
#define COLOR_TASKBAR 0xFF11111C   /* Floating dock */
#define COLOR_TASKBAR_TEXT 0xFFE6E8F5
#define COLOR_ACCENT 0xFF6C63FF       /* cyber-purple */
#define COLOR_ACCENT_HOVER 0xFF8A84FF
#define COLOR_CYAN 0xFF00D4FF         /* cyber-blue */
#define COLOR_PINK 0xFFFF006E         /* cyber-pink */
#define COLOR_NEON_GREEN 0xFF00FF9F   /* cyber-green */
#define COLOR_WIN_BG 0xFF10101A       /* Window background */
#define COLOR_WIN_TITLE 0xFF171726    /* Window title bar / cards */
#define COLOR_WIN_BORDER 0xFF2A2850   /* Window border (purple-tinted) */
#define COLOR_WIN_CLOSE 0xFFFF006E    /* Close button */
#define COLOR_TEXT_WHITE 0xFFFFFFFF
#define COLOR_TEXT_GREEN 0xFF00FF9F  /* Terminal green */
#define COLOR_TEXT_CYAN 0xFF00D4FF   /* Terminal cyan */
#define COLOR_TEXT_YELLOW 0xFFF1FA8C /* Terminal yellow */
#define COLOR_TEXT_GRAY 0xFF8A8DA6
#define COLOR_TEXT_BLACK 0xFF0A0A0F
#define COLOR_CURSOR 0xFFFFFFFF /* Mouse cursor color */
#define COLOR_TRANSPARENT 0x00000000

/* Corner radius of windows */
#define WIN_RADIUS 10

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

/* Small reusable window-manager helpers.  Applications keep their own
 * contents, while these functions provide the common chrome and pointer
 * behaviour (close button, title-bar dragging and screen bounds). */
int gui_window_hit_close(const gui_window_t *win, int32_t x, int32_t y);
int gui_window_begin_drag(gui_window_t *win, int32_t x, int32_t y);
void gui_window_update_drag(gui_window_t *win, int32_t x, int32_t y,
                            int32_t screen_w, int32_t screen_h);
void gui_window_end_drag(gui_window_t *win);

/* Render a button-like widget */
void gui_draw_button(int32_t x, int32_t y, int32_t w, int32_t h,
                     const char *label, uint32_t bg, uint32_t fg);

#endif /* GUI_H */
