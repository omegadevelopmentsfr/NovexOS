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

#endif /* FB_H */
