// Fixed-width bitmap fonts (Spleen, see font_*.c) and UTF-8 helpers.
#pragma once

#include <stdint.h>

typedef struct {
    uint8_t w, h;        // cell size; w <= 8
    uint8_t first, last; // code point range covered
    const uint8_t *bits; // h bytes per glyph, MSB = leftmost pixel
    const uint8_t *missing; // bitmask over first..last: no glyph, use fallback
} ui_font_t;

extern const ui_font_t font_6x12;
extern const ui_font_t font_8x16;

// Decode one code point from UTF-8 at s (not at its NUL). Malformed input
// gives U+FFFD and consumes one byte. Returns the next position.
const char *utf8_next(const char *s, uint32_t *cp);

// Number of code points (= text cells) in s.
int utf8_cells(const char *s);

// Byte offset of the start of cell n in s (or of the NUL if s is shorter).
int utf8_offset(const char *s, int n);

// Glyph rows for cp, or NULL if the font can't draw it.
const uint8_t *font_glyph(const ui_font_t *f, uint32_t cp);
