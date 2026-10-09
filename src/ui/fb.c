#include "fb.h"

#include <string.h>

uint8_t fb[FB_H][FB_STRIDE];

// Not linear: DIM is for de-emphasized text, which has to stay legible.
const uint8_t fb_palette[4][3] = {
    {0x00, 0x00, 0x00},
    {0x60, 0x60, 0x60},
    {0xA8, 0xA8, 0xA8},
    {0xFF, 0xFF, 0xFF},
};

static struct {
    int x0, y0, x1, y1; // half-open
} clip = {0, 0, FB_W, FB_H};

void fb_clip(int x, int y, int w, int h) {
    clip.x0 = x < 0 ? 0 : x;
    clip.y0 = y < 0 ? 0 : y;
    clip.x1 = x + w > FB_W ? FB_W : x + w;
    clip.y1 = y + h > FB_H ? FB_H : y + h;
}

void fb_unclip(void) { fb_clip(0, 0, FB_W, FB_H); }

void fb_clear(uint8_t c) { memset(fb, (c & 3) * 0x55, sizeof fb); }

static inline void put(int x, int y, uint8_t c) {
    uint8_t *p = &fb[y][x >> 2];
    int sh = 6 - 2 * (x & 3);
    *p = (uint8_t)((*p & ~(3 << sh)) | ((c & 3) << sh));
}

void fb_pixel(int x, int y, uint8_t c) {
    if (x >= clip.x0 && x < clip.x1 && y >= clip.y0 && y < clip.y1)
        put(x, y, c);
}

uint8_t fb_get(int x, int y) {
    if (x < 0 || x >= FB_W || y < 0 || y >= FB_H)
        return FB_BLACK;
    return (fb[y][x >> 2] >> (6 - 2 * (x & 3))) & 3;
}

void fb_fill(int x, int y, int w, int h, uint8_t c) {
    int x0 = x < clip.x0 ? clip.x0 : x;
    int y0 = y < clip.y0 ? clip.y0 : y;
    int x1 = x + w > clip.x1 ? clip.x1 : x + w;
    int y1 = y + h > clip.y1 ? clip.y1 : y + h;
    if (x0 >= x1 || y0 >= y1)
        return;
    uint8_t full = (uint8_t)((c & 3) * 0x55);
    for (int yy = y0; yy < y1; yy++) {
        int xx = x0;
        for (; xx < x1 && (xx & 3); xx++)
            put(xx, yy, c);
        int bytes = (x1 - xx) >> 2;
        memset(&fb[yy][xx >> 2], full, bytes);
        xx += bytes * 4;
        for (; xx < x1; xx++)
            put(xx, yy, c);
    }
}

void fb_hline(int x, int y, int w, uint8_t c) { fb_fill(x, y, w, 1, c); }
void fb_vline(int x, int y, int h, uint8_t c) { fb_fill(x, y, 1, h, c); }

void fb_rect(int x, int y, int w, int h, uint8_t c) {
    if (w <= 0 || h <= 0)
        return;
    fb_hline(x, y, w, c);
    fb_hline(x, y + h - 1, w, c);
    fb_vline(x, y + 1, h - 2, c);
    fb_vline(x + w - 1, y + 1, h - 2, c);
}

static void glyph(int x, int y, const uint8_t *g, const ui_font_t *f,
                  uint8_t fg, uint8_t bg) {
    if (bg != FB_CLEAR)
        fb_fill(x, y, f->w, f->h, bg);
    if (g) {
        for (int r = 0; r < f->h; r++)
            for (int col = 0; col < f->w; col++)
                if (g[r] & (0x80 >> col))
                    fb_pixel(x + col, y + r, fg);
    } else {
        // Fallback: a box inside the cell, clear of the neighbours
        fb_rect(x + 1, y + f->h / 6, f->w - 2, f->h - f->h / 3, fg);
    }
}

int fb_text(int x, int y, const char *s, int max_cells, const ui_font_t *f,
            uint8_t fg, uint8_t bg) {
    while (*s && max_cells != 0) {
        uint32_t cp;
        s = utf8_next(s, &cp);
        if (x >= clip.x1)
            break;
        glyph(x, y, font_glyph(f, cp), f, fg, bg);
        x += f->w;
        if (max_cells > 0)
            max_cells--;
    }
    return x;
}

// FNV-1a over each row, compared with what the last fb_flush() saw.
static uint32_t row_hash[FB_H];
static bool hashed;

static uint32_t hash_row(int y) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < FB_STRIDE; i++)
        h = (h ^ fb[y][i]) * 16777619u;
    return h;
}

void fb_invalidate(void) { hashed = false; }

void fb_flush(void (*flush)(int y0, int y1, void *ctx), void *ctx) {
    int start = -1;
    for (int y = 0; y < FB_H; y++) {
        uint32_t h = hash_row(y);
        bool changed = !hashed || h != row_hash[y];
        row_hash[y] = h;
        if (changed && start < 0)
            start = y;
        if (!changed && start >= 0) {
            flush(start, y - 1, ctx);
            start = -1;
        }
    }
    if (start >= 0)
        flush(start, FB_H - 1, ctx);
    hashed = true;
}

void fb_row_rgb565(int y, uint16_t *out) {
    static uint16_t lut[4];
    if (!lut[3])
        for (int i = 0; i < 4; i++)
            lut[i] = (uint16_t)(((fb_palette[i][0] & 0xF8) << 8) |
                                ((fb_palette[i][1] & 0xFC) << 3) |
                                (fb_palette[i][2] >> 3));
    for (int x = 0; x < FB_W; x++)
        out[x] = lut[(fb[y][x >> 2] >> (6 - 2 * (x & 3))) & 3];
}
