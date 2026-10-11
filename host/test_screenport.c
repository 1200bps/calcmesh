// The USB screen mirror (src/screenport.c) against a stand-in USB port
// (fake/fake_tusb.c): decodes what it sends and checks it ends up matching
// the framebuffer, without ever overrunning the port's buffer or sending
// while the port is closed or nothing changed.
//
//   test_screenport [DIR]   also save DIR/stream.bin (the bytes sent after
//                           the last open) and DIR/expect.png, for
//                           tools/screen.py to be checked against
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fake_tusb.h"
#include "fb.h"
#include "png.h"
#include "scene.h"
#include "screenport.h"
#include "ui.h"

static int failures;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
            failures++;                                                        \
        }                                                                      \
    } while (0)

// The PC side: a decoder like tools/screen.py's.
static struct {
    uint8_t rows[FB_H][FB_STRIDE];
    bool seen[FB_H];
    uint8_t palette[12];
    bool got_palette;
    int frames;
    uint8_t pend[4 + FB_STRIDE];
    int npend;
    bool bad;
} pc;

static FILE *stream;

static void pc_reset(void) {
    memset(&pc, 0, sizeof pc);
    if (stream) {
        fflush(stream);
        CHECK(ftruncate(fileno(stream), 0) == 0);
        rewind(stream);
    }
}

static void pc_byte(uint8_t b) {
    pc.pend[pc.npend++] = b;
    if (pc.npend < 4)
        return;
    if (pc.pend[0] != 0xFB) {
        pc.bad = true;
        pc.npend = 0;
        return;
    }
    int y = pc.pend[2] | pc.pend[3] << 8;
    switch (pc.pend[1]) {
    case 'P':
        if (pc.npend < 4 + 12)
            return;
        memcpy(pc.palette, pc.pend + 4, 12);
        pc.got_palette = true;
        break;
    case 'R':
        if (pc.npend < 4 + FB_STRIDE)
            return;
        if (y < FB_H) {
            memcpy(pc.rows[y], pc.pend + 4, FB_STRIDE);
            pc.seen[y] = true;
        } else
            pc.bad = true;
        break;
    case 'E':
        pc.frames++;
        break;
    default:
        pc.bad = true;
    }
    pc.npend = 0;
}

// Read up to chunk bytes off the port, as the PC would.
static uint32_t pc_read(uint32_t chunk) {
    uint8_t buf[FAKE_CDC_BUFSIZE];
    uint32_t n = fake_cdc_drain(buf, chunk);
    if (stream)
        fwrite(buf, 1, n, stream);
    for (uint32_t i = 0; i < n; i++)
        pc_byte(buf[i]);
    return n;
}

static bool pc_matches_fb(void) {
    for (int y = 0; y < FB_H; y++)
        if (!pc.seen[y] || memcmp(pc.rows[y], fb[y], FB_STRIDE))
            return false;
    return true;
}

// One pass of the firmware's main loop, then the PC reads a bit.
static void loop(uint32_t chunk) {
    ui_render();
    fb_flush(screenport_rows, NULL);
    screenport_task();
    CHECK(fake_cdc_len <= FAKE_CDC_BUFSIZE);
    pc_read(chunk);
}

// The PC sends bytes the other way.
static void pc_send(const void *data, uint32_t n) {
    memcpy(fake_cdc_rx + fake_cdc_rx_len, data, n);
    fake_cdc_rx_len += n;
}

static void pc_key(uint16_t key, uint8_t mods) {
    uint8_t p[] = {0xFB, 'K', (uint8_t)key, (uint8_t)(key >> 8), mods};
    pc_send(p, sizeof p);
}

static void press(uint16_t key) {
    kbd_event_t ev = {.key = key};
    ui_key(&ev);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        char path[512];
        snprintf(path, sizeof path, "%s/stream.bin", argv[1]);
        stream = fopen(path, "w+b");
        if (!stream) {
            perror(path);
            return 1;
        }
    }
    ui_init();
    scene_load();

    // Port closed: nothing queued or sent
    for (int i = 0; i < 5; i++)
        loop(4096);
    CHECK(!screenport_open());
    CHECK(fake_cdc_len == 0 && pc.frames == 0);

    // Opened: palette, then the whole screen, in a buffer's worth at a time
    fake_cdc_open = true;
    int passes = 0;
    while (pc.frames == 0 && passes++ < 1000)
        loop(4096);
    CHECK(screenport_open());
    CHECK(pc.got_palette && !memcmp(pc.palette, fb_palette, 12));
    CHECK(pc.frames == 1 && pc_matches_fb() && !pc.bad);

    // Nothing changes, nothing is sent
    for (int i = 0; i < 10; i++)
        loop(4096);
    CHECK(pc.frames == 1);

    // Typing, with a slow reader: only some rows change, the screen still
    // catches up
    press(KBD_TAB);
    press(KBD_ENTER);
    for (const char *s = "hello from the mirror"; *s; s++) {
        press((uint8_t)*s);
        loop(97);
    }
    while (fake_cdc_len || pc.npend)
        loop(97);
    loop(4096);
    CHECK(pc.frames > 1 && pc_matches_fb() && !pc.bad);

    // Closed partway through a frame, then opened again: a clean stream from
    // the start, whatever was left in the buffer
    press(KBD_ESC);
    loop(200);
    fake_cdc_open = false;
    loop(4096);
    CHECK(!screenport_open());
    fake_cdc_len = 0;
    for (int i = 0; i < 5; i++)
        loop(4096);
    CHECK(fake_cdc_len == 0);
    // Garbage the port's buffer held across the reopen
    memset(fake_cdc_buf, 0xFB, 300);
    fake_cdc_len = 300;
    press(KBD_DOWN);
    press(KBD_ENTER);
    fake_cdc_open = true;
    pc_reset();
    passes = 0;
    while (pc.frames == 0 && passes++ < 1000)
        loop(301);
    CHECK(pc.got_palette && pc.frames == 1 && pc_matches_fb() && !pc.bad);

    // Keys from the PC, among junk and split across reads
    kbd_event_t ev;
    CHECK(!screenport_key(&ev));
    pc_send("\xFB\xFBxyz\xFBR", 7);
    pc_key('q', 0);
    pc_key(KBD_F1 + 5, KBD_MOD_SHIFT);
    pc_send("\xFB", 1);
    pc_key('a', KBD_MOD_CTRL | 0x80);
    pc_send("\xFB\x4B\x00", 3); // half a packet
    loop(4096);
    CHECK(fake_cdc_rx_len == 0);
    CHECK(screenport_key(&ev) && ev.key == 'q' && !ev.mods && !ev.repeat);
    CHECK(screenport_key(&ev) && ev.key == KBD_F1 + 5 && ev.mods == KBD_MOD_SHIFT);
    CHECK(screenport_key(&ev) && ev.key == 'a' && ev.mods == KBD_MOD_CTRL &&
          ev.repeat);
    CHECK(!screenport_key(&ev));
    pc_send("\x01\x00", 2); // ...and the rest of it
    loop(4096);
    CHECK(screenport_key(&ev) && ev.key == KBD_UP && !ev.mods);
    CHECK(!screenport_key(&ev));
    // A full queue drops the extras rather than overwriting
    for (int i = 0; i < 20; i++)
        pc_key((uint16_t)('a' + i), 0);
    loop(4096);
    int got = 0;
    while (screenport_key(&ev))
        CHECK(ev.key == 'a' + got++);
    CHECK(got == 16);
    // Closing drops what was queued and any half packet
    pc_key('z', 0);
    pc_send("\xFB\x4B", 2);
    loop(4096);
    fake_cdc_open = false;
    loop(4096);
    fake_cdc_open = true;
    pc_reset();
    pc_send("\x01\x01\x00", 3); // tail of the half packet: ignored
    passes = 0;
    while (pc.frames == 0 && passes++ < 1000)
        loop(4096);
    CHECK(!screenport_key(&ev));
    CHECK(pc.frames == 1 && pc_matches_fb() && !pc.bad);

    if (stream) {
        fclose(stream);
        char path[512];
        snprintf(path, sizeof path, "%s/expect.png", argv[1]);
        CHECK(png_write_fb(path, 1));
    }
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    return failures != 0;
}
