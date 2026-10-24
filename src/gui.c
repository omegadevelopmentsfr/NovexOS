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

  /* Soft shadow, rounded body with a 1px tinted border */
  fb_draw_shadow(x, y, w, h, WIN_RADIUS, 14, 6, 0x000000, 120);
  fb_fill_rrect_border(x, y, w, h, WIN_RADIUS, COLOR_WIN_BG, COLOR_WIN_BORDER);

  /* Title bar (top corners rounded) + purple->cyan accent line */
  fb_fill_rrect_ex(x + 1, y + 1, w - 2, TITLEBAR_HEIGHT - 1, WIN_RADIUS - 1,
                   COLOR_WIN_TITLE, RR_TL | RR_TR);
  fb_fill_gradient_h(x + 1, y + TITLEBAR_HEIGHT - 1, w - 2, 1, COLOR_ACCENT,
                     COLOR_CYAN);

  if (win->title) {
    fb_draw_string_bold(x + 14, y + 6, win->title, COLOR_TEXT_WHITE,
                        COLOR_WIN_TITLE);
  }

  /* Round close button with a smooth cross */
  int32_t close_x = x + w - 22;
  int32_t close_y = y + 6;
  int32_t cx = close_x + 8, cy = close_y + 8;
  fb_fill_circle(cx, cy, 8, COLOR_WIN_CLOSE);
  for (int32_t i = -3; i <= 3; i++) {
    fb_put_pixel(cx + i, cy + i, COLOR_TEXT_WHITE);
    fb_put_pixel(cx + i, cy - i, COLOR_TEXT_WHITE);
    fb_blend_pixel(cx + i + 1, cy + i, 0x80FFFFFF);
    fb_blend_pixel(cx + i - 1, cy + i, 0x80FFFFFF);
    fb_blend_pixel(cx + i + 1, cy - i, 0x80FFFFFF);
    fb_blend_pixel(cx + i - 1, cy - i, 0x80FFFFFF);
  }
}

int gui_window_hit_close(const gui_window_t *win, int32_t px, int32_t py) {
  gui_rect_t close = {win->bounds.x + win->bounds.w - 22,
                      win->bounds.y + 6, 16, 16};
  return win->visible && gui_point_in_rect(px, py, &close);
}

int gui_window_begin_drag(gui_window_t *win, int32_t px, int32_t py) {
  gui_rect_t titlebar = {win->bounds.x, win->bounds.y, win->bounds.w,
                         TITLEBAR_HEIGHT};
  if (!win->visible || !gui_point_in_rect(px, py, &titlebar) ||
      gui_window_hit_close(win, px, py))
    return 0;
  win->dragging = 1;
  win->drag_offset_x = px - win->bounds.x;
  win->drag_offset_y = py - win->bounds.y;
  return 1;
}

void gui_window_update_drag(gui_window_t *win, int32_t px, int32_t py,
                            int32_t screen_w, int32_t screen_h) {
  if (!win->dragging || !win->visible)
    return;
  win->bounds.x = px - win->drag_offset_x;
  win->bounds.y = py - win->drag_offset_y;
  /* Keep a title-bar area accessible, including on smaller resolutions. */
  if (win->bounds.x < 0) win->bounds.x = 0;
  if (win->bounds.y < 0) win->bounds.y = 0;
  if (win->bounds.x > screen_w - 48) win->bounds.x = screen_w - 48;
  if (win->bounds.y > screen_h - TITLEBAR_HEIGHT)
    win->bounds.y = screen_h - TITLEBAR_HEIGHT;
}

void gui_window_end_drag(gui_window_t *win) { win->dragging = 0; }

void gui_draw_button(int32_t x, int32_t y, int32_t w, int32_t h,
                     const char *label, uint32_t bg, uint32_t fg) {
  /* Dark fills keep a subtle border; bright fills get none. */
  uint32_t lum = (((bg >> 16) & 0xFF) * 30 + ((bg >> 8) & 0xFF) * 59 +
                  (bg & 0xFF) * 11) / 100;
  uint32_t border = (lum < 70) ? COLOR_WIN_BORDER : bg;
  /* Keep text readable on bright neon fills */
  if (lum > 140)
    fg = COLOR_TEXT_BLACK;
  fb_fill_rrect_border(x, y, w, h, h / 4 + 2, bg, border);

  /* Center text */
  int32_t text_w = fb_text_width(label);
  int32_t tx = x + (w - text_w) / 2;
  int32_t ty = y + (h - FONT_HEIGHT) / 2;
  fb_draw_string_bold(tx, ty, label, fg, bg);
}
