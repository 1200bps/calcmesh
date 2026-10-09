#include "screenport.h"

#include <string.h>

#include "tusb.h"
#include "ui/fb.h"

#define CDC SCREENPORT_USB_CDC
#define HDR 4

static uint8_t dirty[(FB_H + 7) / 8];
static int ndirty;
static int cursor;       // where the scan for the next dirty row starts
static bool frame_end;   // owe an 'E' once the dirty rows are out
static bool palette_due; // owe a 'P'
static bool open;

static void mark(int y) {
    if (!(dirty[y >> 3] & (1 << (y & 7)))) {
        dirty[y >> 3] |= (uint8_t)(1 << (y & 7));
        ndirty++;
    }
}

void screenport_rows(int y0, int y1, void *ctx) {
    (void)ctx;
    if (!open)
        return;
    for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < FB_H; y++)
        mark(y);
    frame_end = true;
}

bool screenport_open(void) { return open; }

static void header(uint8_t type, uint16_t arg) {
    uint8_t h[HDR] = {0xFB, type, (uint8_t)arg, (uint8_t)(arg >> 8)};
    tud_cdc_n_write(CDC, h, HDR);
}

void screenport_task(void) {
    bool now_open = tud_cdc_n_connected(CDC);
    if (now_open && !open) {
        // Whatever was left in the buffer from an earlier reader would
        // arrive mid-packet; start clean and send everything.
        tud_cdc_n_write_clear(CDC);
        for (int y = 0; y < FB_H; y++)
            mark(y);
        palette_due = frame_end = true;
    } else if (!now_open && open) {
        memset(dirty, 0, sizeof dirty);
        ndirty = 0;
        palette_due = frame_end = false;
    }
    open = now_open;
    if (!open)
        return;
    tud_cdc_n_read_flush(CDC); // nothing to read yet

    bool sent = false;
    if (palette_due && tud_cdc_n_write_available(CDC) >= HDR + 12) {
        header('P', 0);
        tud_cdc_n_write(CDC, fb_palette, 12);
        palette_due = false;
        sent = true;
    }
    while (!palette_due && ndirty &&
           tud_cdc_n_write_available(CDC) >= HDR + FB_STRIDE) {
        int y = cursor;
        while (!(dirty[y >> 3] & (1 << (y & 7))))
            y = (y + 1) % FB_H;
        dirty[y >> 3] &= (uint8_t)~(1 << (y & 7));
        ndirty--;
        cursor = (y + 1) % FB_H;
        header('R', (uint16_t)y);
        tud_cdc_n_write(CDC, fb[y], FB_STRIDE);
        sent = true;
    }
    if (!palette_due && !ndirty && frame_end &&
        tud_cdc_n_write_available(CDC) >= HDR) {
        header('E', 0);
        frame_end = false;
        sent = true;
    }
    if (sent)
        tud_cdc_n_write_flush(CDC);
}
