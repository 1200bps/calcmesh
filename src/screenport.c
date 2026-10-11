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

// Keys from the PC: a partly read packet, then a queue for screenport_key().
#define KEY_PKT (HDR + 1)
static uint8_t rx[KEY_PKT];
static int nrx;
static kbd_event_t keys[16];
static uint8_t key_head, key_count;

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

bool screenport_key(kbd_event_t *ev) {
    if (!key_count)
        return false;
    *ev = keys[key_head];
    key_head = (uint8_t)((key_head + 1) % 16);
    key_count--;
    return true;
}

static void rx_byte(uint8_t b) {
    // Resync on the magic byte; anything but a 'K' packet is dropped.
    if (nrx == 0 && b != 0xFB)
        return;
    if (nrx == 1 && b != 'K') {
        nrx = b == 0xFB;
        return;
    }
    rx[nrx++] = b;
    if (nrx < KEY_PKT)
        return;
    nrx = 0;
    if (key_count == 16)
        return; // nobody's reading them; drop
    keys[(key_head + key_count++) % 16] = (kbd_event_t){
        .key = (uint16_t)(rx[2] | rx[3] << 8),
        .mods = rx[4] & (KBD_MOD_SHIFT | KBD_MOD_CTRL | KBD_MOD_ALT),
        .repeat = rx[4] & 0x80,
    };
}

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
        nrx = key_count = 0;
    }
    open = now_open;
    if (!open)
        return;
    uint8_t in[64];
    uint32_t n;
    while ((n = tud_cdc_n_read(CDC, in, sizeof in)) > 0)
        for (uint32_t i = 0; i < n; i++)
            rx_byte(in[i]);

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
