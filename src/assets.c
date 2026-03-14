/*
 * NovexOS - assets.c
 * Sprite/asset registry.
 *
 * To add a new PNG asset:
 *   1. Place the .png in src/assets/<category>/  (or src/assets/ for globals)
 *   2. Run  make assets
 *   3. #include the generated header here and fill in a sprite_t entry.
 */

#include "assets.h"

/* --- Generated pixel data ------------------------------------------------ */
#include "assets/background_img.h"
#include "assets/mouse_pointer/pointer_img.h"

/* --- Cursor sprites ------------------------------------------------------- */

static const sprite_t cursor_sprites[CURSOR_COUNT] = {
    /* CURSOR_POINTER — tip at (0,0) */
    {
        .width = POINTER_IMG_W,
        .height = POINTER_IMG_H,
        .hotspot_x = 0,
        .hotspot_y = 0,
        .pixels = pointer_img_pixels,
    },
};

const sprite_t *assets_get_cursor(cursor_type_t type) {
  if ((int)type < 0 || type >= CURSOR_COUNT)
    return &cursor_sprites[CURSOR_POINTER];
  return &cursor_sprites[type];
}

/* --- Background sprite ---------------------------------------------------- */

static const sprite_t background_sprite = {
    .width = BACKGROUND_IMG_W,
    .height = BACKGROUND_IMG_H,
    .hotspot_x = 0,
    .hotspot_y = 0,
    .pixels = background_img_pixels,
};

const sprite_t *assets_get_background(void) { return &background_sprite; }