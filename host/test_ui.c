// Framebuffer primitives, UTF-8 handling, and UI flows driven by key events
// against the made-up mesh in fake/scene.c.
#include <stdio.h>
#include <string.h>

#include "msgs.h"
#include "fb.h"
#include "nodedb.h"
#include "scene.h"
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

static int count_shade(int x, int y, int w, int h, uint8_t c) {
    int n = 0;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            n += fb_get(xx, yy) == c;
    return n;
}

static void test_fill_and_clip(void) {
    fb_unclip();
    fb_clear(FB_BLACK);
    fb_fill(3, 5, 10, 2, FB_GREY); // unaligned on both ends
    CHECK(count_shade(0, 0, FB_W, FB_H, FB_GREY) == 20);
    CHECK(fb_get(2, 5) == FB_BLACK && fb_get(3, 5) == FB_GREY);
    CHECK(fb_get(12, 6) == FB_GREY && fb_get(13, 6) == FB_BLACK);

    // Off-screen and partly off-screen drawing is clipped, not a crash
    fb_clear(FB_BLACK);
    fb_fill(-50, -50, 60, 60, FB_WHITE);
    CHECK(count_shade(0, 0, FB_W, FB_H, FB_WHITE) == 100);
    fb_rect(FB_W - 2, FB_H - 2, 10, 10, FB_DIM);
    fb_hline(-1000, 400, 5000, FB_WHITE);
    fb_text(FB_W - 3, FB_H - 4, "clipped", -1, &font_8x16, FB_WHITE, FB_GREY);

    fb_clear(FB_BLACK);
    fb_clip(10, 10, 5, 5);
    fb_fill(0, 0, FB_W, FB_H, FB_WHITE);
    CHECK(count_shade(0, 0, FB_W, FB_H, FB_WHITE) == 25);
    fb_unclip();
}

static void test_text(void) {
    fb_clear(FB_BLACK);
    int x = fb_text(0, 0, "A", -1, &font_6x12, FB_WHITE, FB_CLEAR);
    CHECK(x == 6);
    // Spleen 6x12 'A': row 1 is .###.. , row 5 is #####.
    CHECK(fb_get(0, 1) == FB_BLACK && fb_get(1, 1) == FB_WHITE &&
          fb_get(3, 1) == FB_WHITE && fb_get(4, 1) == FB_BLACK);
    CHECK(count_shade(0, 5, 6, 1, FB_WHITE) == 5);

    // max_cells limits; a multi-byte character is one cell
    CHECK(fb_text(0, 20, "héllo", 2, &font_6x12, FB_WHITE, FB_CLEAR) == 12);
    CHECK(fb_text(0, 20, "héllo", -1, &font_6x12, FB_WHITE, FB_CLEAR) == 30);

    // No glyph: a box, not nothing
    fb_clear(FB_BLACK);
    fb_text(0, 0, "\xE2\x9C\x93", -1, &font_6x12, FB_WHITE, FB_CLEAR); // U+2713
    CHECK(count_shade(0, 0, 6, 12, FB_WHITE) > 8);
    CHECK(font_glyph(&font_6x12, 0x2713) == NULL);
    CHECK(font_glyph(&font_6x12, 0x85) == NULL); // C1 control
    CHECK(font_glyph(&font_6x12, 0xF1) != NULL); // ñ

    // Background fill
    fb_clear(FB_BLACK);
    fb_text(0, 0, " ", -1, &font_8x16, FB_WHITE, FB_GREY);
    CHECK(count_shade(0, 0, 8, 16, FB_GREY) == 128);
}

static void test_utf8(void) {
    uint32_t cp;
    const char *s = "a\xC3\xB1\xE2\x82\xAC\xF0\x9F\x93\xA1";
    s = utf8_next(s, &cp);
    CHECK(cp == 'a');
    s = utf8_next(s, &cp);
    CHECK(cp == 0xF1);
    s = utf8_next(s, &cp);
    CHECK(cp == 0x20AC);
    s = utf8_next(s, &cp);
    CHECK(cp == 0x1F4E1 && *s == '\0');

    // Truncated sequence: replacement, one byte at a time, never past the NUL
    const char *bad = "\xE2\x82";
    const char *p = utf8_next(bad, &cp);
    CHECK(cp == 0xFFFD && p == bad + 1);
    CHECK(utf8_cells("\xE2\x82") == 2);
    CHECK(utf8_cells("a\xC3\xB1z") == 3);
    CHECK(utf8_offset("a\xC3\xB1z", 2) == 3);
    CHECK(utf8_offset("ab", 5) == 2);
}

static int runs, run_rows;
static void count_runs(int y0, int y1, void *ctx) {
    (void)ctx;
    runs++;
    run_rows += y1 - y0 + 1;
}

static void test_flush(void) {
    fb_clear(FB_BLACK);
    fb_invalidate();
    runs = run_rows = 0;
    fb_flush(count_runs, NULL);
    CHECK(runs == 1 && run_rows == FB_H);

    runs = run_rows = 0;
    fb_flush(count_runs, NULL);
    CHECK(runs == 0);

    fb_pixel(5, 10, FB_WHITE);
    fb_fill(0, 100, 4, 3, FB_DIM);
    fb_pixel(319, 319, FB_GREY);
    runs = run_rows = 0;
    fb_flush(count_runs, NULL);
    CHECK(runs == 3 && run_rows == 5);

    uint16_t row[FB_W];
    fb_row_rgb565(10, row);
    CHECK(row[5] == 0xFFFF && row[4] == 0x0000);
}

static void press(uint16_t key) {
    kbd_event_t ev = {.key = key};
    ui_key(&ev);
    ui_render();
}

static void type(const char *s) {
    while (*s)
        press((uint8_t)*s++);
}

static const mesh_msg_t *last(mesh_conv_t c) { return mesh_msgs_recent(c, 0); }

static int unread(mesh_conv_t c) {
    const mesh_conv_info_t *ci = mesh_msgs_conv(c);
    return ci ? ci->unread : 0;
}

static void test_flows(void) {
    ui_init();
    scene_load();
    ui_render();
    CHECK(ui_tab() == UI_TAB_NODES);

    press(KBD_TAB);
    CHECK(ui_tab() == UI_TAB_CHATS);
    kbd_event_t back = {.key = KBD_TAB, .mods = KBD_MOD_SHIFT};
    ui_key(&back);
    CHECK(ui_tab() == UI_TAB_NODES);
    press(KBD_F1 + 1);
    CHECK(ui_tab() == UI_TAB_CHATS);
    press(KBD_F1 + 4); // empty slot
    CHECK(ui_tab() == UI_TAB_CHATS);

    // Opening #LongFast marks it read
    mesh_conv_t lf = MESH_CONV_CHANNEL(0);
    CHECK(unread(lf) == 3);
    press(KBD_ENTER);
    CHECK(unread(lf) == 0);

    // Compose: insert, cursor moves, backspace, delete
    type("hix");
    press(KBD_BACKSPACE);
    press(KBD_LEFT);
    type("X");
    press(KBD_HOME);
    press(KBD_DEL);
    press(KBD_END);
    type("!");
    press(KBD_ENTER);
    const mesh_msg_t *m = last(lf);
    CHECK(m && strcmp(m->text, "Xi!") == 0);
    CHECK(m && m->from == SCENE_ME && m->status == MESH_MSG_QUEUED);

    // Empty Enter sends nothing
    int n = mesh_msgs_count(lf);
    press(KBD_ENTER);
    CHECK(mesh_msgs_count(lf) == n);

    // A failed send keeps the draft for another try
    fake_send_result = MESH_SEND_NOT_READY;
    type("retry me");
    press(KBD_ENTER);
    CHECK(mesh_msgs_count(lf) == n);
    fake_send_result = MESH_SEND_OK;
    press(KBD_ENTER);
    CHECK(mesh_msgs_count(lf) == n + 1 && strcmp(last(lf)->text, "retry me") == 0);

    // The draft stops at the protocol limit
    for (int i = 0; i < MESH_MSG_SEND_MAX + 20; i++)
        press('a' + i % 26);
    press(KBD_ENTER);
    CHECK(strlen(last(lf)->text) == MESH_MSG_SEND_MAX);

    // Scrolling far past either end is harmless
    for (int i = 0; i < 50; i++)
        press(KBD_PGUP);
    for (int i = 0; i < 50; i++)
        press(KBD_DOWN);

    // Esc back to the list; Nodes -> Enter on a node opens a DM with it
    press(KBD_ESC);
    press(KBD_F1);
    press(KBD_DOWN); // first row is our own node
    press(KBD_ENTER);
    CHECK(ui_tab() == UI_TAB_CHATS);
    type("dm");
    press(KBD_ENTER);
    mesh_conv_t dm = MESH_CONV_DM(scene_node(0)); // Ridge Repeater, top favourite
    CHECK(last(dm) && strcmp(last(dm)->text, "dm") == 0 &&
          last(dm)->to == scene_node(0));
}

// A full table with long, odd names, rendered under the sanitizers
static void test_stress(void) {
    ui_init();
    scene_load();
    for (int i = 0; mesh_db.node_count < MESH_MAX_NODES; i++) {
        meshtastic_NodeInfo ni = meshtastic_NodeInfo_init_zero;
        ni.num = 0x50000000u + (uint32_t)i;
        ni.has_user = i % 3 != 0;
        memset(ni.user.long_name, i % 2 ? 'W' : '\xC3', sizeof ni.user.long_name - 1);
        strcpy(ni.user.short_name, "\xF0\x9F\x93\xA1"); // one emoji
        ni.last_heard = SCENE_NOW - (uint32_t)i * 977;
        mesh_db_upsert_node(&ni);
    }
    for (int i = 0; i < 300; i++) {
        char text[MESH_MSG_TEXT_MAX + 1];
        memset(text, i % 2 ? 'x' : ' ', MESH_MSG_TEXT_MAX);
        text[MESH_MSG_TEXT_MAX] = '\0';
        uint32_t from = 0x50000000u + (uint32_t)i;
        scene_msg(from, i % 3 ? MESH_CONV_CHANNEL(0) : MESH_CONV_DM(from), 0,
                  MESH_MSG_RECEIVED, text);
    }
    ui_render();
    press(KBD_END);
    press(KBD_PGUP);
    press(KBD_TAB);
    press(KBD_ENTER);
    press(KBD_PGUP);
    press(KBD_END);
}

int main(void) {
    test_fill_and_clip();
    test_text();
    test_utf8();
    test_flush();
    test_flows();
    test_stress();
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    else
        printf("ok\n");
    return failures != 0;
}
