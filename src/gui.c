/*
 * NovexOS - gui.c
 * GUI widget rendering for NovexDE.
 */

#include "gui.h"
#include "fb.h"
#include "font.h"
#include "string.h"

int gui_point_in_rect(int32_t px, int32_t py, const gui_rect_t *r) {
  return (px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h);
}

void gui_window_init(gui_window_t *win, int32_t x, int32_t y, int32_t w,
                     int32_t h, const char *title) {
  win->bounds.x = x;
  win->bounds.y = y;
  win->bounds.w = w;
  win->bounds.h = h;
  win->title = title;
  win->visible = 1;
  win->dragging = 0;
  win->drag_offset_x = 0;
  win->drag_offset_y = 0;
}

void gui_window_render(const gui_window_t *win) {
  if (!win->visible)
    return;

  int32_t x = win->bounds.x;
  int32_t y = win->bounds.y;
  int32_t w = win->bounds.w;
  int32_t h = win->bounds.h;

  /* Shadow (offset by 3px) */
  fb_fill_rect(x + 3, y + 3, w, h, 0x80000000);

  /* Window border */
  fb_fill_rect(x - 1, y - 1, w + 2, h + 2, COLOR_WIN_BORDER);

  /* Window background */
  fb_fill_rect(x, y, w, h, COLOR_WIN_BG);

  /* Title bar */
  fb_fill_rect(x, y, w, TITLEBAR_HEIGHT, COLOR_WIN_TITLE);

  /* Title bar accent stripe (2px at the very top) */
  fb_fill_rect(x, y, w, 2, COLOR_ACCENT);

  /* Title text */
  if (win->title) {
    fb_draw_string(x + 10, y + 6, win->title, COLOR_TEXT_WHITE,
                   COLOR_WIN_TITLE);
  }

  /* Close button (circle in top-right) */
  int32_t close_x = x + w - 22;
  int32_t close_y = y + 6;
  fb_fill_rect(close_x, close_y, 16, 16, COLOR_WIN_CLOSE);
  /* Draw 'x' on close button */
  fb_draw_char(close_x + 4, close_y, 'x', COLOR_TEXT_WHITE, COLOR_WIN_CLOSE);
}

void gui_draw_button(int32_t x, int32_t y, int32_t w, int32_t h,
                     const char *label, uint32_t bg, uint32_t fg) {
  fb_fill_rect(x, y, w, h, bg);
  fb_draw_rect(x, y, w, h, COLOR_WIN_BORDER);

  /* Center text */
  int32_t text_len = (int32_t)strlen(label);
  int32_t text_w = text_len * FONT_WIDTH;
  int32_t tx = x + (w - text_w) / 2;
  int32_t ty = y + (h - FONT_HEIGHT) / 2;
  fb_draw_string(tx, ty, label, fg, bg);
}
