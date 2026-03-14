/*
 * NovexOS - assets.h
 * PNG sprite system — assets are converted to C headers at build time
 * by tools/png_to_header.py (via 'make assets').
 *
 * Cursor
 * ------
 *   CURSOR_POINTER  — arrow pointer (pointer.png, 32x32)
 *
 * Background
 * ----------
 *   assets_get_background() — desktop wallpaper (background.png, 1024x768)
 *   Blit it with fb_draw_background() — when the image matches the screen
 *   resolution (default VBE: 1024x768) this is a single memcpy with zero
 *   scaling overhead.
 *   using nearest-neighbor — much faster than a per-frame gradient loop.
 */

#ifndef ASSETS_H
#define ASSETS_H

#include "types.h"

/* -------------------------------------------------------------------------
 * Generic sprite descriptor
 * ------------------------------------------------------------------------- */
typedef struct {
  uint32_t width;         /* Sprite width  in pixels */
  uint32_t height;        /* Sprite height in pixels */
  int32_t hotspot_x;      /* Click-point offset from left edge */
  int32_t hotspot_y;      /* Click-point offset from top  edge */
  const uint32_t *pixels; /* ARGB pixel data (0xAARRGGBB) */
} sprite_t;

/* -------------------------------------------------------------------------
 * Cursor types
 * ------------------------------------------------------------------------- */
typedef enum { CURSOR_POINTER = 0, CURSOR_COUNT } cursor_type_t;

/* Return the sprite descriptor for the requested cursor.
 * Never returns NULL — falls back to CURSOR_POINTER on invalid input. */
const sprite_t *assets_get_cursor(cursor_type_t type);

/* Return the background wallpaper sprite (1024x768, fully opaque). */
const sprite_t *assets_get_background(void);

#endif /* ASSETS_H */