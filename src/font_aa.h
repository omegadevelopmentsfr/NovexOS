/*
 * NovexOS - font_aa.h
 * Anti-aliased 8x16 font (JetBrains Mono) covering English + French.
 *
 * Text is stored as single bytes in Windows-1252 (CP1252): ASCII plus
 * é è ê à ç ù î ô û ü ï ö œ É È À Ç Œ « » € ’ ... in 0xA0-0xFF / 0x80-0x9F.
 * fb_draw_string() additionally decodes UTF-8 so C string literals can be
 * written naturally ("Redémarrer").
 */

#ifndef FONT_AA_H
#define FONT_AA_H

#include "types.h"

/* 128 bytes (8 columns x 16 rows) of 0..255 coverage for CP1252 byte c. */
const uint8_t *font_aa_glyph(uint8_t c, int bold);

/* Unicode code point -> CP1252 byte ('?' when the font has no glyph). */
uint8_t font_unicode_to_cp1252(uint32_t cp);

#endif /* FONT_AA_H */
