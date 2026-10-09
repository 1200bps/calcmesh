// The screen as an in-memory framebuffer: 320x320, 2 bits per pixel (four
// shades of grey), plus the drawing primitives the UI is built from.
//
// Nothing here knows about the LCD. A display driver reads the buffer out
// with fb_flush(), which hands it only the rows that changed since its last
// call, and converts shades to its own pixel format with fb_palette or
// fb_row_rgb565() / fb_row_rgb666(). On the host, the same buffer is saved as a PNG.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "font.h"

#define FB_W 320
#define FB_H 320
#define FB_STRIDE (FB_W / 4) // bytes per row; pixel x is bits 7-6 of byte x/4 at x%4 == 0

enum {
    FB_BLACK = 0,
    FB_DIM = 1,
    FB_GREY = 2,
    FB_WHITE = 3,
    FB_CLEAR = 0xFF, // as a text background: leave it alone
};

extern uint8_t fb[FB_H][FB_STRIDE];

// RGB888 for each shade.
extern const uint8_t fb_palette[4][3];

// Drawing is clipped to this rectangle (the whole screen by default).
void fb_clip(int x, int y, int w, int h);
void fb_unclip(void);

void fb_clear(uint8_t c);
void fb_pixel(int x, int y, uint8_t c);
uint8_t fb_get(int x, int y);
void fb_hline(int x, int y, int w, uint8_t c);
void fb_vline(int x, int y, int h, uint8_t c);
void fb_rect(int x, int y, int w, int h, uint8_t c); // outline
void fb_fill(int x, int y, int w, int h, uint8_t c);

// Draw UTF-8 text with its cell's top-left at (x, y), at most max_cells
// cells (< 0: no limit). Code points the font lacks are drawn as a box.
// Returns the x after the last cell drawn.
int fb_text(int x, int y, const char *s, int max_cells, const ui_font_t *f,
            uint8_t fg, uint8_t bg);

// Call flush(y0, y1) (inclusive) for each run of rows that changed since the
// previous fb_flush(); everything counts as changed on the first call.
void fb_flush(void (*flush)(int y0, int y1, void *ctx), void *ctx);

// Mark every row changed, e.g. after the panel was reset.
void fb_invalidate(void);

// One row as RGB565 (FB_W pixels), for panels that take that format.
void fb_row_rgb565(int y, uint16_t *out);

// One row as RGB666 (FB_W pixels, 3 bytes each: R, G, B with the colour in
// bits 7-2), the only full-colour format an ILI9488 takes over 4-wire SPI.
void fb_row_rgb666(int y, uint8_t *out);
