/*
 * NovexOS - fb.c
 * Framebuffer drawing primitives with double buffering.
 * All drawing operates on a back-buffer; fb_swap() blits to hardware.
 *
 * Optimisations over the original:
 *   - fb_clear / fb_fill_rect / fb_hline  use memset32 (2 pixels per store)
 *   - fb_draw_char: fast path with unrolled 8-pixel rows, no per-pixel
 *     bounds check when the glyph is fully within screen bounds
 *   - fb_swap: single memcpy when pitch == width*4 (common VBE mode)
 *   - fb_draw_sprite_alpha: division by 255 replaced by multiply-shift
 *   - fb_draw_background: existing memcpy fast path kept
 */

#include "fb.h"
#include "font.h"
#include "string.h"

/* Back-buffer — static allocation in BSS (max 1920x1080x4 = ~8.3 MB) */
#define FB_MAX_W 1920
#define FB_MAX_H 1080
static uint32_t backbuffer[FB_MAX_W * FB_MAX_H];

static uint32_t *hw_fb    = NULL;
static uint32_t  scr_w    = 0;
static uint32_t  scr_h    = 0;
static uint32_t  scr_pitch = 0; /* pitch in bytes */

void fb_init(uint32_t *hw_buffer, uint32_t width, uint32_t height,
             uint32_t pitch) {
  hw_fb     = hw_buffer;
  scr_w     = (width  > FB_MAX_W) ? FB_MAX_W : width;
  scr_h     = (height > FB_MAX_H) ? FB_MAX_H : height;
  scr_pitch = pitch;
  memset(backbuffer, 0, scr_w * scr_h * sizeof(uint32_t));
}

/* Fill entire back-buffer with one colour. */
void fb_clear(uint32_t color) {
  memset32(backbuffer, color, scr_w * scr_h);
}

/* -------------------------------------------------------------------------
 * Internal: write a pixel with bounds check (used only by safe fallbacks)
 * ------------------------------------------------------------------------- */
static inline void set_pixel(int32_t x, int32_t y, uint32_t color) {
  if (x >= 0 && x < (int32_t)scr_w && y >= 0 && y < (int32_t)scr_h)
    backbuffer[y * scr_w + x] = color;
}

void fb_put_pixel(int32_t x, int32_t y, uint32_t color) {
  set_pixel(x, y, color);
}

/* -------------------------------------------------------------------------
 * fb_fill_rect — clipped filled rectangle.
 * Uses memset32 per row: 2 pixels per 64-bit store instead of 1.
 * ------------------------------------------------------------------------- */
void fb_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
  int32_t x0 = (x < 0) ? 0 : x;
  int32_t y0 = (y < 0) ? 0 : y;
  int32_t x1 = x + w;
  int32_t y1 = y + h;
  if (x1 > (int32_t)scr_w) x1 = (int32_t)scr_w;
  if (y1 > (int32_t)scr_h) y1 = (int32_t)scr_h;
  if (x1 <= x0 || y1 <= y0) return;

  uint32_t run = (uint32_t)(x1 - x0);
  for (int32_t py = y0; py < y1; py++)
    memset32(backbuffer + py * scr_w + x0, color, run);
}

void fb_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
  fb_hline(x, y,         w, color);
  fb_hline(x, y + h - 1, w, color);
  fb_vline(x,         y, h, color);
  fb_vline(x + w - 1, y, h, color);
}

/* -------------------------------------------------------------------------
 * fb_hline — clipped horizontal line using memset32.
 * Clips once, then one vectorised fill — no per-pixel set_pixel overhead.
 * ------------------------------------------------------------------------- */
void fb_hline(int32_t x, int32_t y, int32_t w, uint32_t color) {
  if (y < 0 || y >= (int32_t)scr_h || w <= 0) return;
  int32_t x0 = (x < 0) ? 0 : x;
  int32_t x1 = x + w;
  if (x1 > (int32_t)scr_w) x1 = (int32_t)scr_w;
  if (x1 <= x0) return;
  memset32(backbuffer + y * scr_w + x0, color, (uint32_t)(x1 - x0));
}

void fb_vline(int32_t x, int32_t y, int32_t h, uint32_t color) {
  if (x < 0 || x >= (int32_t)scr_w || h <= 0) return;
  int32_t y0 = (y < 0) ? 0 : y;
  int32_t y1 = y + h;
  if (y1 > (int32_t)scr_h) y1 = (int32_t)scr_h;
  for (int32_t py = y0; py < y1; py++)
    backbuffer[py * scr_w + x] = color;
}

/* -------------------------------------------------------------------------
 * fb_draw_char — bitmap font glyph renderer.
 *
 * Fast path (glyph fully within screen bounds):
 *   - Computes a direct pointer to the destination row once per row.
 *   - Unrolls all 8 pixel writes — no loop overhead, no branch per pixel.
 *   - No bounds check per pixel.
 *
 * Safe path (glyph partially outside screen):
 *   - Falls back to set_pixel() with per-pixel clipping.
 * ------------------------------------------------------------------------- */
void fb_draw_char(int32_t x, int32_t y, char c, uint32_t fg, uint32_t bg) {
  const uint8_t *glyph = font_get_glyph(c);

  /* Fast path: entire glyph within screen */
  if (x >= 0 && y >= 0 &&
      (uint32_t)x + FONT_WIDTH  <= scr_w &&
      (uint32_t)y + FONT_HEIGHT <= scr_h) {
    for (int32_t row = 0; row < FONT_HEIGHT; row++) {
      uint8_t   line = glyph[row];
      uint32_t *dst  = backbuffer + (y + row) * scr_w + x;
      dst[0] = (line & 0x80) ? fg : bg;
      dst[1] = (line & 0x40) ? fg : bg;
      dst[2] = (line & 0x20) ? fg : bg;
      dst[3] = (line & 0x10) ? fg : bg;
      dst[4] = (line & 0x08) ? fg : bg;
      dst[5] = (line & 0x04) ? fg : bg;
      dst[6] = (line & 0x02) ? fg : bg;
      dst[7] = (line & 0x01) ? fg : bg;
    }
    return;
  }

  /* Safe path: clipped */
  for (int32_t row = 0; row < FONT_HEIGHT; row++) {
    uint8_t line = glyph[row];
    for (int32_t col = 0; col < FONT_WIDTH; col++)
      set_pixel(x + col, y + row, (line & (0x80 >> col)) ? fg : bg);
  }
}

int32_t fb_draw_string(int32_t x, int32_t y, const char *str, uint32_t fg,
                       uint32_t bg) {
  while (*str) {
    if (*str == '\n') {
      y += FONT_HEIGHT;
      x = 0;
    } else {
      fb_draw_char(x, y, *str, fg, bg);
      x += FONT_WIDTH;
    }
    str++;
  }
  return x;
}

/* -------------------------------------------------------------------------
 * fb_swap — copy backbuffer to hardware framebuffer.
 *
 * Fast path: if pitch == width*4, the hardware buffer is a contiguous array
 * and a single memcpy suffices (no per-row dispatch overhead).
 * ------------------------------------------------------------------------- */
void fb_swap(void) {
  if (!hw_fb) return;

  uint32_t line_bytes = scr_w * 4;

  /* Fast path: contiguous layout */
  if (scr_pitch == line_bytes) {
    memcpy(hw_fb, backbuffer, (size_t)scr_h * line_bytes);
    return;
  }

  /* Slow path: non-contiguous (pitch > width*4) */
  uint8_t *dst = (uint8_t *)hw_fb;
  uint8_t *src = (uint8_t *)backbuffer;
  for (uint32_t y = 0; y < scr_h; y++) {
    memcpy(dst, src, line_bytes);
    dst += scr_pitch;
    src += line_bytes;
  }
}

uint32_t *fb_get_backbuffer(void) { return backbuffer; }
uint32_t  fb_get_width(void)      { return scr_w; }
uint32_t  fb_get_height(void)     { return scr_h; }

/* -------------------------------------------------------------------------
 * fb_draw_background — nearest-neighbor stretch blit.
 *
 * Fast path: source dimensions match screen → single memcpy (zero math).
 * Slow path: fixed-point nearest-neighbor scaling.
 * Alpha byte is forced to 0xFF — background is always fully opaque.
 * ------------------------------------------------------------------------- */
void fb_draw_background(const uint32_t *pixels, uint32_t img_w, uint32_t img_h)
{
  /* Fast path: exact size match — one memcpy, no scaling */
  if (img_w == scr_w && img_h == scr_h) {
    memcpy(backbuffer, pixels, scr_w * scr_h * sizeof(uint32_t));
    return;
  }

  /* Slow path: nearest-neighbor stretch */
  uint32_t step_x = (img_w << 16) / scr_w;
  uint32_t step_y = (img_h << 16) / scr_h;

  uint32_t src_y = 0;
  for (uint32_t dy = 0; dy < scr_h; dy++, src_y += step_y) {
    const uint32_t *src_row = pixels + (src_y >> 16) * img_w;
    uint32_t       *dst_row = backbuffer + dy * scr_w;
    uint32_t        src_x   = 0;
    for (uint32_t dx = 0; dx < scr_w; dx++, src_x += step_x)
      dst_row[dx] = src_row[src_x >> 16] | 0xFF000000u;
  }
}

/* -------------------------------------------------------------------------
 * fb_draw_sprite_alpha — per-pixel alpha-blended sprite renderer.
 *
 * Pixel format: 0xAARRGGBB.
 *
 * fg_color: if non-zero, RGB channels are replaced by fg_color's RGB while
 *           the per-pixel alpha from the sprite is preserved.  Lets black
 *           silhouette cursors be tinted to any colour.
 *
 * outline: if non-zero, draws a 1-pixel border of that colour around every
 *          sprite pixel whose alpha >= 32 but whose NESW neighbour is fully
 *          transparent.  Adds a readable edge to cursor sprites.
 *
 * Division by 255 uses the multiply-shift identity:
 *   x/255 ≈ (x + (x>>8) + 1) >> 8   (exact for x in [0, 65535])
 * This replaces an expensive integer divide with two adds and two shifts.
 * ------------------------------------------------------------------------- */

/* Fast approximate x/255, exact for x in [0, 65535] */
static inline uint32_t div255(uint32_t x) {
  return (x + (x >> 8) + 1) >> 8;
}

void fb_draw_sprite_alpha(int32_t sx, int32_t sy,
                          const uint32_t *pixels,
                          uint32_t w, uint32_t h,
                          uint32_t fg_color,
                          uint32_t outline)
{
  uint8_t fg_r = (uint8_t)(fg_color >> 16);
  uint8_t fg_g = (uint8_t)(fg_color >>  8);
  uint8_t fg_b = (uint8_t)(fg_color      );

  for (uint32_t row = 0; row < h; row++) {
    int32_t py = sy + (int32_t)row;
    if (py < 0 || py >= (int32_t)scr_h) continue;

    for (uint32_t col = 0; col < w; col++) {
      int32_t px = sx + (int32_t)col;
      if (px < 0 || px >= (int32_t)scr_w) continue;

      uint32_t src = pixels[row * w + col];
      uint8_t  sa  = (uint8_t)(src >> 24);

      if (sa == 0) {
        /* Transparent pixel — draw outline if neighbour is opaque */
        if (outline) {
          int border =
            (col > 0     && (pixels[row * w + col - 1] >> 24) >= 32) ||
            (col + 1 < w && (pixels[row * w + col + 1] >> 24) >= 32) ||
            (row > 0     && (pixels[(row-1) * w + col] >> 24) >= 32) ||
            (row + 1 < h && (pixels[(row+1) * w + col] >> 24) >= 32);
          if (border)
            backbuffer[py * scr_w + px] = outline;
        }
        continue;
      }

      /* Source RGB — use tint or raw */
      uint8_t sr = fg_color ? fg_r : (uint8_t)(src >> 16);
      uint8_t sg = fg_color ? fg_g : (uint8_t)(src >>  8);
      uint8_t sb = fg_color ? fg_b : (uint8_t)(src      );

      if (sa == 255) {
        /* Fully opaque — no blend needed */
        backbuffer[py * scr_w + px] =
          0xFF000000u | ((uint32_t)sr << 16) | ((uint32_t)sg << 8) | sb;
      } else {
        /* Alpha blend: out = (sa*src + inv*dst) / 255 */
        uint32_t dst = backbuffer[py * scr_w + px];
        uint8_t  dr  = (uint8_t)(dst >> 16);
        uint8_t  dg  = (uint8_t)(dst >>  8);
        uint8_t  db  = (uint8_t)(dst      );
        uint8_t  inv = (uint8_t)(255u - sa);

        uint8_t or_ = (uint8_t)div255(sa * sr + inv * dr);
        uint8_t og  = (uint8_t)div255(sa * sg + inv * dg);
        uint8_t ob  = (uint8_t)div255(sa * sb + inv * db);

        backbuffer[py * scr_w + px] =
          0xFF000000u | ((uint32_t)or_ << 16) | ((uint32_t)og << 8) | ob;
      }
    }
  }
}
