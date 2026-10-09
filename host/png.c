#include "png.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fb.h"

static uint32_t crc_table[256];

static uint32_t crc32(uint32_t crc, const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len)
        fwrite(data, 1, len, f);
    uint32_t crc = crc32(crc32(0, hdr + 4, 4), data, len);
    be32(hdr, crc);
    fwrite(hdr, 1, 4, f);
}

bool png_write_fb(const char *path, int scale) {
    if (scale < 1)
        scale = 1;
    int w = FB_W * scale, h = FB_H * scale;
    size_t row = (size_t)w + 1; // filter byte + grey
    size_t raw_len = row * (size_t)h;
    uint8_t *raw = malloc(raw_len);
    // zlib stream: header, stored blocks of <= 65535 bytes, adler32
    size_t blocks = (raw_len + 65534) / 65535;
    uint8_t *z = malloc(2 + raw_len + blocks * 5 + 4);
    if (!raw || !z) {
        free(raw);
        free(z);
        return false;
    }

    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * row;
        r[0] = 0; // no filter
        for (int x = 0; x < w; x++)
            r[1 + x] = fb_palette[fb_get(x / scale, y / scale)][0];
    }

    size_t zn = 0;
    z[zn++] = 0x78;
    z[zn++] = 0x01;
    for (size_t off = 0; off < raw_len; off += 65535) {
        size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
        z[zn++] = off + n == raw_len; // BFINAL, BTYPE=00
        z[zn++] = (uint8_t)n;
        z[zn++] = (uint8_t)(n >> 8);
        z[zn++] = (uint8_t)~n;
        z[zn++] = (uint8_t)(~n >> 8);
        memcpy(z + zn, raw + off, n);
        zn += n;
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    be32(z + zn, (b << 16) | a);
    zn += 4;

    FILE *f = fopen(path, "wb");
    bool ok = f != NULL;
    if (ok) {
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        uint8_t ihdr[13];
        be32(ihdr, (uint32_t)w);
        be32(ihdr + 4, (uint32_t)h);
        ihdr[8] = 8;  // bit depth
        ihdr[9] = 0;  // greyscale
        ihdr[10] = 0; // deflate
        ihdr[11] = 0; // adaptive filtering
        ihdr[12] = 0; // no interlace
        fwrite(sig, 1, 8, f);
        chunk(f, "IHDR", ihdr, sizeof ihdr);
        chunk(f, "IDAT", z, (uint32_t)zn);
        chunk(f, "IEND", NULL, 0);
        ok = fclose(f) == 0;
    }
    free(raw);
    free(z);
    return ok;
}
