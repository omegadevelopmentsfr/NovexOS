/*
 * NovexOS - fb.h
 * Framebuffer drawing primitives — double-buffered.
 */

#ifndef FB_H
#define FB_H

#include "types.h"

/* Initialize the framebuffer drawing system */
void fb_init(uint32_t *hw_buffer, uint32_t width, uint32_t height,
             uint32_t pitch);

/* Clear to a single color */
void fb_clear(uint32_t color);

/* Filled rectangle */
void fb_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);

/* Outlined rectangle */
void fb_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);

/* Horizontal line */
void fb_hline(int32_t x, int32_t y, int32_t w, uint32_t color);

/* Vertical line */
void fb_vline(int32_t x, int32_t y, int32_t h, uint32_t color);

/* Single pixel */
void fb_put_pixel(int32_t x, int32_t y, uint32_t color);

/* Draw a single character at (x,y) using bitmap font */
void fb_draw_char(int32_t x, int32_t y, char c, uint32_t fg, uint32_t bg);

/* Draw a string at (x,y), returns X position after the last character */
int32_t fb_draw_string(int32_t x, int32_t y, const char *str, uint32_t fg,
                       uint32_t bg);

/* Copy back-buffer to hardware framebuffer */
void fb_swap(void);

/* Get back-buffer pointer and dimensions */
uint32_t *fb_get_backbuffer(void);
uint32_t fb_get_width(void);
uint32_t fb_get_height(void);

/*
 * Stretch a pre-baked background image to fill the entire screen using
 * nearest-neighbor scaling.  No alpha blending — every pixel is overwritten.
 * Much faster than a per-frame gradient loop.
 *
 * pixels[]   — RGB/ARGB pixel array (0xAARRGGBB), img_w * img_h entries.
 * img_w/h    — source image dimensions (e.g. 320x240).
 */
void fb_draw_background(const uint32_t *pixels, uint32_t img_w, uint32_t img_h);

/*
 * Draw a sprite with per-pixel alpha blending.
 *
 * pixels[]  — ARGB pixel array (0xAARRGGBB), width*height entries.
 * fg_color  — foreground tint applied to opaque pixels (0xAARRGGBB).
 *             Pass 0x00000000 to use the raw pixel colours from the sprite.
 * outline   — colour drawn around fully-transparent pixels that are
 *             adjacent to opaque ones (0 = no outline).
 *
 * Alpha blending formula (per channel):
 *   out = (src_alpha * src + (255 - src_alpha) * dst) / 255
 */
void fb_draw_sprite_alpha(int32_t x, int32_t y, const uint32_t *pixels,
                          uint32_t w, uint32_t h, uint32_t fg_color,
                          uint32_t outline);


/* Bold variant and text measurement (UTF-8 aware: counts characters). */
int32_t fb_draw_string_bold(int32_t x, int32_t y, const char *str, uint32_t fg,
                            uint32_t bg);
int32_t fb_text_width(const char *str);

/* ---- Modern primitives (alpha / rounded / shadow / gradient) ---- */
#define RR_TL 1
#define RR_TR 2
#define RR_BL 4
#define RR_BR 8
#define RR_ALL 15
void fb_blend_pixel(int32_t x, int32_t y, uint32_t argb);
void fb_blend_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t argb);
void fb_fill_rrect_ex(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r,
                      uint32_t color, int corners);
void fb_fill_rrect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r,
                   uint32_t color);
void fb_fill_rrect_border(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r,
                          uint32_t fill, uint32_t border);
void fb_fill_circle(int32_t cx, int32_t cy, int32_t rad, uint32_t color);
void fb_draw_shadow(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r,
                    int32_t spread, int32_t off_y, uint32_t rgb, uint32_t max_a);
void fb_fill_gradient_v(int32_t x, int32_t y, int32_t w, int32_t h,
                        uint32_t c0, uint32_t c1);
void fb_fill_gradient_h(int32_t x, int32_t y, int32_t w, int32_t h,
                        uint32_t c0, uint32_t c1);
void fb_draw_cyber_background(void);

#endif /* FB_H */