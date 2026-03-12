/*
 * NovexOS - fb.c
 * Framebuffer drawing primitives with double buffering.
 * All drawing operates on a back-buffer; fb_swap() blits to hardware.
 */

#include "fb.h"
#include "font.h"
#include "string.h"

/* Back-buffer — static allocation in BSS (max 1920×1080×4 = ~8.3 MB) */
#define FB_MAX_W 1920
#define FB_MAX_H 1080
static uint32_t backbuffer[FB_MAX_W * FB_MAX_H];

static uint32_t *hw_fb = NULL;
static uint32_t scr_w = 0;
static uint32_t scr_h = 0;
static uint32_t scr_pitch = 0; /* pitch in bytes */

void fb_init(uint32_t *hw_buffer, uint32_t width, uint32_t height,
             uint32_t pitch) {
  hw_fb = hw_buffer;
  scr_w = (width > FB_MAX_W) ? FB_MAX_W : width;
  scr_h = (height > FB_MAX_H) ? FB_MAX_H : height;
  scr_pitch = pitch;
  memset(backbuffer, 0, sizeof(backbuffer));
}

void fb_clear(uint32_t color) {
  uint32_t total = scr_w * scr_h;
  for (uint32_t i = 0; i < total; i++) {
    backbuffer[i] = color;
  }
}

static inline void set_pixel(int32_t x, int32_t y, uint32_t color) {
  if (x >= 0 && x < (int32_t)scr_w && y >= 0 && y < (int32_t)scr_h) {
    backbuffer[y * scr_w + x] = color;
  }
}

void fb_put_pixel(int32_t x, int32_t y, uint32_t color) {
  set_pixel(x, y, color);
}

void fb_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
  /* Clip */
  int32_t x0 = (x < 0) ? 0 : x;
  int32_t y0 = (y < 0) ? 0 : y;
  int32_t x1 = x + w;
  int32_t y1 = y + h;
  if (x1 > (int32_t)scr_w)
    x1 = (int32_t)scr_w;
  if (y1 > (int32_t)scr_h)
    y1 = (int32_t)scr_h;

  for (int32_t py = y0; py < y1; py++) {
    for (int32_t px = x0; px < x1; px++) {
      backbuffer[py * scr_w + px] = color;
    }
  }
}

void fb_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
  fb_hline(x, y, w, color);
  fb_hline(x, y + h - 1, w, color);
  fb_vline(x, y, h, color);
  fb_vline(x + w - 1, y, h, color);
}

void fb_hline(int32_t x, int32_t y, int32_t w, uint32_t color) {
  for (int32_t i = 0; i < w; i++) {
    set_pixel(x + i, y, color);
  }
}

void fb_vline(int32_t x, int32_t y, int32_t h, uint32_t color) {
  for (int32_t i = 0; i < h; i++) {
    set_pixel(x, y + i, color);
  }
}

void fb_draw_char(int32_t x, int32_t y, char c, uint32_t fg, uint32_t bg) {
  const uint8_t *glyph = font_get_glyph(c);
  for (int32_t row = 0; row < FONT_HEIGHT; row++) {
    uint8_t line = glyph[row];
    for (int32_t col = 0; col < FONT_WIDTH; col++) {
      uint32_t color = (line & (0x80 >> col)) ? fg : bg;
      set_pixel(x + col, y + row, color);
    }
  }
}

int32_t fb_draw_string(int32_t x, int32_t y, const char *str, uint32_t fg,
                       uint32_t bg) {
  while (*str) {
    if (*str == '\n') {
      y += FONT_HEIGHT;
      x = 0; /* Reset to left margin */
    } else {
      fb_draw_char(x, y, *str, fg, bg);
      x += FONT_WIDTH;
    }
    str++;
  }
  return x;
}

void fb_swap(void) {
  if (!hw_fb)
    return;

  /* Copy back-buffer to hardware framebuffer, line by line
   * (pitch may differ from width * 4) */
  uint32_t line_bytes = scr_w * 4;
  uint8_t *dst = (uint8_t *)hw_fb;
  uint8_t *src = (uint8_t *)backbuffer;

  for (uint32_t y = 0; y < scr_h; y++) {
    memcpy(dst, src, line_bytes);
    dst += scr_pitch;
    src += line_bytes;
  }
}

uint32_t *fb_get_backbuffer(void) { return backbuffer; }
uint32_t fb_get_width(void) { return scr_w; }
uint32_t fb_get_height(void) { return scr_h; }
