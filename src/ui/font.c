#include "font.h"

#include <stddef.h>

const char *utf8_next(const char *s, uint32_t *cp) {
    const uint8_t *p = (const uint8_t *)s;
    uint32_t c = p[0];
    int n;
    if (c < 0x80) {
        *cp = c;
        return s + 1;
    } else if ((c & 0xE0) == 0xC0) {
        n = 1;
        c &= 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        n = 2;
        c &= 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        n = 3;
        c &= 0x07;
    } else {
        *cp = 0xFFFD;
        return s + 1;
    }
    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return s + 1;
        }
        c = (c << 6) | (p[i] & 0x3F);
    }
    *cp = c;
    return s + 1 + n;
}

int utf8_cells(const char *s) {
    int n = 0;
    uint32_t cp;
    while (*s) {
        s = utf8_next(s, &cp);
        n++;
    }
    return n;
}

int utf8_offset(const char *s, int n) {
    const char *p = s;
    uint32_t cp;
    while (*p && n-- > 0)
        p = utf8_next(p, &cp);
    return (int)(p - s);
}

const uint8_t *font_glyph(const ui_font_t *f, uint32_t cp) {
    if (cp < f->first || cp > f->last)
        return NULL;
    unsigned i = cp - f->first;
    if (f->missing[i >> 3] & (1u << (i & 7)))
        return NULL;
    return f->bits + i * f->h;
}
