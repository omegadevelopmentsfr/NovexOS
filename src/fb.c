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
#include "font_aa.h"
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

/* ---- Colour helpers (integer only, kernel-safe) ---- */
static inline uint32_t div255(uint32_t x) { return (x + (x >> 8) + 1) >> 8; }

static uint32_t isqrt_u32(uint32_t n) {
  uint32_t r = 0, bit = 1u << 30;
  while (bit > n) bit >>= 2;
  while (bit) {
    if (n >= r + bit) { n -= r + bit; r = (r >> 1) + bit; }
    else r >>= 1;
    bit >>= 2;
  }
  return r;
}

/* Blend src over dst with alpha a (0..255). Result is opaque. */
static inline uint32_t mix_px(uint32_t dst, uint32_t src, uint32_t a) {
  if (a == 0) return dst;
  if (a >= 255) return 0xFF000000u | (src & 0x00FFFFFFu);
  uint32_t ia = 255u - a;
  uint32_t r = div255(((src >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * ia);
  uint32_t g = div255(((src >> 8) & 0xFF) * a + ((dst >> 8) & 0xFF) * ia);
  uint32_t b = div255((src & 0xFF) * a + (dst & 0xFF) * ia);
  return 0xFF000000u | (r << 16) | (g << 8) | b;
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
/* ---- Text: anti-aliased JetBrains Mono (CP1252 cells, UTF-8 strings) ---- */

/* Decode one character of a (possibly UTF-8) string into a CP1252 byte.
 * Returns the number of source bytes consumed. Invalid UTF-8 is treated as
 * raw CP1252, so single-byte strings (keyboard input) keep working. */
static int text_next(const unsigned char *s, uint8_t *out) {
  unsigned char c = s[0];
  if (c < 0x80) { *out = c; return 1; }
  if (c >= 0xC2 && c <= 0xDF && (s[1] & 0xC0) == 0x80) {
    *out = font_unicode_to_cp1252(((uint32_t)(c & 0x1F) << 6) | (s[1] & 0x3F));
    return 2;
  }
  if (c >= 0xE0 && c <= 0xEF && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
    *out = font_unicode_to_cp1252(((uint32_t)(c & 0x0F) << 12) |
                                  ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F));
    return 3;
  }
  *out = c;
  return 1;
}

static void draw_cell(int32_t x, int32_t y, uint8_t b, uint32_t fg,
                      uint32_t bg, int bold) {
  const uint8_t *g = font_aa_glyph(b, bold);
  int inside = (x >= 0 && y >= 0 && (uint32_t)x + FONT_WIDTH <= scr_w &&
                (uint32_t)y + FONT_HEIGHT <= scr_h);
  for (int row = 0; row < FONT_HEIGHT; row++) {
    for (int col = 0; col < FONT_WIDTH; col++) {
      uint32_t a = g[row * FONT_WIDTH + col];
      uint32_t px = (a == 0) ? bg : mix_px(bg, fg, a);
      if (inside)
        backbuffer[(y + row) * scr_w + x + col] = px;
      else
        set_pixel(x + col, y + row, px);
    }
  }
}

void fb_draw_char(int32_t x, int32_t y, char c, uint32_t fg, uint32_t bg) {
  draw_cell(x, y, (uint8_t)c, fg, bg, 0);
}

int32_t fb_draw_string_ex(int32_t x, int32_t y, const char *str, uint32_t fg,
                          uint32_t bg, int bold) {
  const unsigned char *s = (const unsigned char *)str;
  while (*s) {
    if (*s == '\n') {
      y += FONT_HEIGHT;
      x = 0;
      s++;
      continue;
    }
    uint8_t b;
    s += text_next(s, &b);
    draw_cell(x, y, b, fg, bg, bold);
    x += FONT_WIDTH;
  }
  return x;
}

int32_t fb_draw_string(int32_t x, int32_t y, const char *str, uint32_t fg,
                       uint32_t bg) {
  return fb_draw_string_ex(x, y, str, fg, bg, 0);
}

int32_t fb_draw_string_bold(int32_t x, int32_t y, const char *str, uint32_t fg,
                            uint32_t bg) {
  return fb_draw_string_ex(x, y, str, fg, bg, 1);
}

/* Width in pixels of a (UTF-8 or CP1252) string. */
int32_t fb_text_width(const char *str) {
  const unsigned char *s = (const unsigned char *)str;
  int32_t n = 0;
  uint8_t b;
  while (*s) {
    s += text_next(s, &b);
    n++;
  }
  return n * FONT_WIDTH;
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

/* =========================================================================
 * Modern 2D primitives: alpha, anti-aliased rounded rects, shadows,
 * gradients and the generated cyber wallpaper. Integer maths only.
 * ========================================================================= */

static void blend_px(int32_t x, int32_t y, uint32_t color, uint32_t a) {
  if (x < 0 || y < 0 || x >= (int32_t)scr_w || y >= (int32_t)scr_h) return;
  uint32_t *d = &backbuffer[y * scr_w + x];
  *d = mix_px(*d, color, a);
}

void fb_blend_pixel(int32_t x, int32_t y, uint32_t argb) {
  blend_px(x, y, argb, argb >> 24);
}

/* Fill [x0,x1) on row y; honours the alpha byte of color. */
static void span_fill(int32_t x0, int32_t x1, int32_t y, uint32_t color) {
  if (y < 0 || y >= (int32_t)scr_h) return;
  if (x0 < 0) x0 = 0;
  if (x1 > (int32_t)scr_w) x1 = (int32_t)scr_w;
  if (x1 <= x0) return;
  uint32_t a = color >> 24;
  if (a == 0) return;
  uint32_t *dst = backbuffer + y * scr_w + x0;
  if (a >= 255) {
    memset32(dst, color | 0xFF000000u, (size_t)(x1 - x0));
    return;
  }
  for (int32_t i = 0; i < x1 - x0; i++) dst[i] = mix_px(dst[i], color, a);
}

void fb_blend_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t argb) {
  for (int32_t i = 0; i < h; i++) span_fill(x, x + w, y + i, argb);
}

/* One anti-aliased corner quadrant. (bx,by) = top-left of the r*r box. */
static void rr_corner(int32_t bx, int32_t by, int32_t r, int right, int bottom,
                      uint32_t color) {
  uint32_t base = color >> 24;
  for (int32_t j = 0; j < r; j++) {
    int32_t b = bottom ? j + 1 : r - j;
    for (int32_t i = 0; i < r; i++) {
      int32_t a = right ? i + 1 : r - i;
      uint32_t d2 = (uint32_t)((2 * a - 1) * (2 * a - 1) + (2 * b - 1) * (2 * b - 1));
      int32_t d16 = (int32_t)isqrt_u32(d2 * 64u);
      int32_t cov = r * 16 + 8 - d16;
      if (cov <= 0) continue;
      if (cov > 16) cov = 16;
      blend_px(bx + i, by + j, color, ((uint32_t)cov * base) >> 4);
    }
  }
}

void fb_fill_rrect_ex(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r,
                      uint32_t color, int corners) {
  if (w <= 0 || h <= 0) return;
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  if (r < 0) r = 0;
  for (int32_t row = 0; row < h; row++) {
    int32_t x0 = x, x1 = x + w;
    if (row < r) {
      if (corners & RR_TL) x0 = x + r;
      if (corners & RR_TR) x1 = x + w - r;
    } else if (row >= h - r) {
      if (corners & RR_BL) x0 = x + r;
      if (corners & RR_BR) x1 = x + w - r;
    }
    span_fill(x0, x1, y + row, color);
  }
  if (r == 0) return;
  if (corners & RR_TL) rr_corner(x, y, r, 0, 0, color);
  if (corners & RR_TR) rr_corner(x + w - r, y, r, 1, 0, color);
  if (corners & RR_BL) rr_corner(x, y + h - r, r, 0, 1, color);
  if (corners & RR_BR) rr_corner(x + w - r, y + h - r, r, 1, 1, color);
}

void fb_fill_rrect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r,
                   uint32_t color) {
  fb_fill_rrect_ex(x, y, w, h, r, color, RR_ALL);
}

void fb_fill_rrect_border(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r,
                          uint32_t fill, uint32_t border) {
  fb_fill_rrect_ex(x, y, w, h, r, border, RR_ALL);
  fb_fill_rrect_ex(x + 1, y + 1, w - 2, h - 2, r > 0 ? r - 1 : 0, fill, RR_ALL);
}

void fb_fill_circle(int32_t cx, int32_t cy, int32_t rad, uint32_t color) {
  fb_fill_rrect_ex(cx - rad, cy - rad, rad * 2, rad * 2, rad, color, RR_ALL);
}

/* Horizontal extent of a rounded rect on row yy; returns 0 if row outside. */
static int rr_span(int32_t X, int32_t Y, int32_t W, int32_t H, int32_t R,
                   int32_t yy, int32_t *x0, int32_t *x1) {
  if (yy < Y || yy >= Y + H) return 0;
  int32_t row = yy - Y, d = 0;
  if (row < R) d = R - row;
  else if (row >= H - R) d = row - (H - R) + 1;
  int32_t inset = 0;
  if (d > 0) {
    int32_t t = 2 * d - 1;
    int32_t q = 4 * R * R - t * t;
    if (q < 0) q = 0;
    inset = R - ((int32_t)isqrt_u32((uint32_t)q) + 1) / 2;
    if (inset < 0) inset = 0;
  }
  *x0 = X + inset;
  *x1 = X + W - inset;
  return 1;
}

/* Soft drop shadow built from disjoint alpha rings (each pixel blended once). */
void fb_draw_shadow(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r,
                    int32_t spread, int32_t off_y, uint32_t rgb,
                    uint32_t max_a) {
  int32_t sy = y + off_y;
  rgb &= 0x00FFFFFFu;
  for (int32_t k = 0; k <= spread; k++) {
    uint32_t q = (uint32_t)(spread - k + 1);
    uint32_t den = (uint32_t)((spread + 1) * (spread + 1));
    uint32_t a = (max_a * q * q) / den;
    if (a == 0) continue;
    uint32_t col = (a << 24) | rgb;
    int32_t X = x - k, Y = sy - k, W = w + 2 * k, H = h + 2 * k, R = r + k;
    int32_t row0 = Y, row1 = Y + H;
    if (k == 0) {
      row0 = y + h - r - 1;
      if (row0 < Y) row0 = Y;
    }
    for (int32_t yy = row0; yy < row1; yy++) {
      int32_t o0, o1, i0, i1;
      if (!rr_span(X, Y, W, H, R, yy, &o0, &o1)) continue;
      if (k > 0 && rr_span(X + 1, Y + 1, W - 2, H - 2, R - 1 > 0 ? R - 1 : 0,
                           yy, &i0, &i1)) {
        span_fill(o0, i0, yy, col);
        span_fill(i1, o1, yy, col);
      } else {
        span_fill(o0, o1, yy, col);
      }
    }
  }
}

static uint32_t lerp_rgb(uint32_t c0, uint32_t c1, uint32_t t, uint32_t tmax) {
  uint32_t r = (((c0 >> 16) & 0xFF) * (tmax - t) + ((c1 >> 16) & 0xFF) * t) / tmax;
  uint32_t g = (((c0 >> 8) & 0xFF) * (tmax - t) + ((c1 >> 8) & 0xFF) * t) / tmax;
  uint32_t b = ((c0 & 0xFF) * (tmax - t) + (c1 & 0xFF) * t) / tmax;
  return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void fb_fill_gradient_v(int32_t x, int32_t y, int32_t w, int32_t h,
                        uint32_t c0, uint32_t c1) {
  uint32_t tm = (h > 1) ? (uint32_t)(h - 1) : 1u;
  for (int32_t i = 0; i < h; i++)
    span_fill(x, x + w, y + i, lerp_rgb(c0, c1, (uint32_t)i, tm));
}

void fb_fill_gradient_h(int32_t x, int32_t y, int32_t w, int32_t h,
                        uint32_t c0, uint32_t c1) {
  uint32_t tm = (w > 1) ? (uint32_t)(w - 1) : 1u;
  for (int32_t i = 0; i < w; i++) {
    uint32_t col = lerp_rgb(c0, c1, (uint32_t)i, tm);
    for (int32_t j = 0; j < h; j++) set_pixel(x + i, y + j, col);
  }
}

/* ---- Cyber wallpaper: gradient + two soft glows + faint grid ----------
 * Colours come from the NovexOS website (cyber-black / purple / blue).
 * The 2D field is sampled on a coarse 32px lattice and interpolated, so a
 * full redraw costs a few adds per pixel and needs no big cache buffer. */
#define WP_CELL 32
#define WP_COLS_MAX 62
#define WP_ROWS_MAX 36
static int32_t wp_c[3][WP_ROWS_MAX][WP_COLS_MAX]; /* 8.8 fixed, r/g/b */
static uint32_t wp_w, wp_h;

static uint32_t wp_glow(int32_t px, int32_t py, int32_t cx, int32_t cy,
                        int32_t rx, int32_t ry) {
  int32_t dx = px - cx, dy = py - cy;
  if (dx < 0) dx = -dx;
  if (dy < 0) dy = -dy;
  if (dx >= rx || dy >= ry) return 0;
  uint32_t ux = ((uint32_t)dx * 256u) / (uint32_t)rx;
  uint32_t uy = ((uint32_t)dy * 256u) / (uint32_t)ry;
  uint32_t qx = 256u - ((ux * ux) >> 8), qy = 256u - ((uy * uy) >> 8);
  uint32_t tx = (qx * qx) >> 8, ty = (qy * qy) >> 8;
  return (tx * ty) >> 8; /* 0..256 */
}

static void wp_build(void) {
  int32_t W = (int32_t)scr_w, H = (int32_t)scr_h;
  int32_t cols = (W + WP_CELL - 1) / WP_CELL + 1;
  int32_t rows = (H + WP_CELL - 1) / WP_CELL + 1;
  for (int32_t gy = 0; gy < rows; gy++) {
    for (int32_t gx = 0; gx < cols; gx++) {
      int32_t px = gx * WP_CELL, py = gy * WP_CELL;
      uint32_t t = (uint32_t)((py > H ? H : py) * 256 / H);
      uint32_t base = lerp_rgb(0xFF0A0A0F, 0xFF0E0B22, t, 256);
      uint32_t a1 = (wp_glow(px, py, W * 22 / 100, H * 20 / 100, W * 55 / 100,
                             H * 70 / 100) * 66u) >> 8;
      uint32_t a2 = (wp_glow(px, py, W * 82 / 100, H * 88 / 100, W * 50 / 100,
                             H * 65 / 100) * 38u) >> 8;
      uint32_t c = mix_px(base, 0xFF6C63FF, a1);
      c = mix_px(c, 0xFF00D4FF, a2);
      wp_c[0][gy][gx] = (int32_t)(((c >> 16) & 0xFF) << 8);
      wp_c[1][gy][gx] = (int32_t)(((c >> 8) & 0xFF) << 8);
      wp_c[2][gy][gx] = (int32_t)((c & 0xFF) << 8);
    }
  }
  wp_w = scr_w;
  wp_h = scr_h;
}

void fb_draw_cyber_background(void) {
  if (!scr_w || !scr_h) return;
  if (wp_w != scr_w || wp_h != scr_h) wp_build();
  int32_t W = (int32_t)scr_w, H = (int32_t)scr_h;
  int32_t cols = (W + WP_CELL - 1) / WP_CELL + 1;
  int32_t rowv[3][WP_COLS_MAX];

  for (int32_t y = 0; y < H; y++) {
    int32_t gy = y / WP_CELL, fy = y % WP_CELL;
    for (int c = 0; c < 3; c++)
      for (int32_t gx = 0; gx < cols; gx++)
        rowv[c][gx] = (wp_c[c][gy][gx] * (WP_CELL - fy) +
                       wp_c[c][gy + 1][gx] * fy) / WP_CELL;
    uint32_t *dst = backbuffer + y * scr_w;
    int32_t dy = (y & 1) * 128;
    for (int32_t gx = 0; gx < cols - 1; gx++) {
      int32_t x0 = gx * WP_CELL;
      if (x0 >= W) break;
      int32_t n = W - x0 < WP_CELL ? W - x0 : WP_CELL;
      int32_t r = rowv[0][gx], g = rowv[1][gx], b = rowv[2][gx];
      int32_t sr = (rowv[0][gx + 1] - r) / WP_CELL;
      int32_t sg = (rowv[1][gx + 1] - g) / WP_CELL;
      int32_t sb = (rowv[2][gx + 1] - b) / WP_CELL;
      for (int32_t i = 0; i < n; i++) {
        int32_t d = ((i & 1) * 64) + dy; /* 2x2 ordered dither */
        dst[x0 + i] = 0xFF000000u | (uint32_t)(((r + d) >> 8) << 16) |
                      (uint32_t)(((g + d) >> 8) << 8) | (uint32_t)((b + d) >> 8);
        r += sr; g += sg; b += sb;
      }
    }
  }

  /* Faint cyber grid (like .grid-bg on the website) */
  const uint32_t gcol = (9u << 24) | 0x0000D4FFu;
  for (int32_t y = 0; y < H; y += 64) span_fill(0, W, y, gcol);
  for (int32_t x = 0; x < W; x += 64)
    for (int32_t y = 0; y < H; y++) blend_px(x, y, gcol, 9);
}
