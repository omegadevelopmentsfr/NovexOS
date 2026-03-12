/*
 * NovexOS - font.h
 * 8×16 bitmap font for framebuffer text rendering.
 */

#ifndef FONT_H
#define FONT_H

#include "types.h"

#define FONT_WIDTH 8
#define FONT_HEIGHT 16

/* Get pointer to the 16-byte glyph bitmap for character c (ASCII 32-127) */
const uint8_t *font_get_glyph(char c);

#endif /* FONT_H */
