// Framebuffer primitives, UTF-8 handling, and UI flows driven by key events
// against the made-up mesh in fake/scene.c.
#include <stdio.h>
#include <string.h>

#include "mesh/nodeedit.h"
#include "mesh/remoteadmin.h"
#include "msgs.h"
#include "fb.h"
#include "sd/settings.h"
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

    uint8_t row666[FB_W * 3];
    fb_row_rgb666(10, row666);
    CHECK(row666[15] == 0xFC && row666[16] == 0xFC && row666[17] == 0xFC);
    CHECK(row666[12] == 0 && row666[18] == 0);
    fb_row_rgb666(100, row666);
    CHECK(row666[9] == (fb_palette[FB_DIM][0] & 0xFC) && row666[12] == 0);
    fb_row_rgb666(319, row666);
    CHECK(row666[319 * 3] == (fb_palette[FB_GREY][0] & 0xFC));
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
    press(KBD_F1 + 3); // empty slot
    CHECK(ui_tab() == UI_TAB_CHATS);
    press(KBD_TAB); // skips the empty slots
    CHECK(ui_tab() == UI_TAB_SETTINGS);
    press(KBD_TAB);
    CHECK(ui_tab() == UI_TAB_NODES);
    ui_key(&back);
    CHECK(ui_tab() == UI_TAB_SETTINGS);
    press(KBD_F1 + 1);

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

static void downs(int n) {
    for (int i = 0; i < n; i++)
        press(KBD_DOWN);
}

// The node answers a write: resyncs with whatever settings it now has
static void node_back(void) {
    fake_link_stats.syncs++;
    fake_link_state = MESH_LINK_READY;
    mesh_edit_poll();
    ui_render();
}

static void test_settings_tab(void) {
    ui_init();
    scene_load();
    mesh_edit_poll();
    fake_admin_sent = 0;
    press(KBD_F1 + 4);
    CHECK(ui_tab() == UI_TAB_SETTINGS);
    ui_render();

    // Rows: whose settings, long name, short name, role, NodeInfo, region, ...
    downs(5);
    press(KBD_RIGHT);
    CHECK(mesh_edit.lora.region == meshtastic_Config_LoRaConfig_RegionCode_EU_433);
    CHECK(mesh_edit_base.lora.region == meshtastic_Config_LoRaConfig_RegionCode_US);
    press(KBD_LEFT);
    press(KBD_LEFT); // never to Unset: wraps to the last region
    CHECK(mesh_edit.lora.region != meshtastic_Config_LoRaConfig_RegionCode_UNSET);
    press(KBD_RIGHT);
    CHECK(mesh_edit_dirty() == 0);
    press(KBD_RIGHT);
    CHECK(mesh_edit_dirty() == MESH_EDIT_LORA);
    press(KBD_UP); // NodeInfo: default (3h), then steps of its own
    press(KBD_RIGHT);
    CHECK(mesh_edit.device.node_info_broadcast_secs == 3600);
    press(KBD_RIGHT);
    CHECK(mesh_edit.device.node_info_broadcast_secs == 7200);
    press(KBD_LEFT), press(KBD_LEFT), press(KBD_LEFT);
    CHECK(mesh_edit.device.node_info_broadcast_secs == 0);
    CHECK(mesh_edit_dirty() == MESH_EDIT_LORA);

    // Text: Enter edits, Tab and F-keys stay in the field, Esc cancels
    press(KBD_HOME);
    press(KBD_DOWN); // past whose settings
    press(KBD_ENTER);
    press(KBD_TAB);
    press(KBD_F1);
    CHECK(ui_tab() == UI_TAB_SETTINGS);
    for (int i = 0; i < 10; i++)
        press(KBD_BACKSPACE);
    type("Calc 12Tc");
    press(KBD_ESC);
    CHECK(strcmp(mesh_edit.owner.long_name, "PicoCalc") == 0);
    press(KBD_ENTER);
    for (int i = 0; i < 10; i++)
        press(KBD_BACKSPACE);
    press(KBD_ENTER); // empty: refused, still editing
    type("Calc 12Tc");
    for (int i = 0; i < 60; i++)
        press('x'); // stops at the field's size
    for (int i = 0; i < 60; i++)
        press(KBD_BACKSPACE);
    type("  Calc 12Tc ");
    press(KBD_ENTER);
    CHECK(strcmp(mesh_edit.owner.long_name, "Calc 12Tc") == 0);
    CHECK(mesh_edit_dirty() == (MESH_EDIT_LORA | MESH_EDIT_OWNER));
    ui_render();

    // Write: asks first; N doesn't write, Y does
    // long, short, role, nodeinfo, region, preset, hops, tx power, transmit, 3 channels,
    // gps, interval, smart, fixed, power saving, bluetooth, then Write
    downs(18);
    press(KBD_ENTER);
    ui_render();
    press(KBD_F1); // the question holds the keys too
    CHECK(ui_tab() == UI_TAB_SETTINGS);
    press('n');
    CHECK(fake_admin_sent == 0);
    press(KBD_ENTER);
    press('y');
    // begin, set_owner, set_config(lora), commit
    CHECK(fake_admin_sent == 4);
    CHECK(mesh_edit_state() == MESH_EDIT_RESTARTING);
    ui_render();

    // Comes back with both: confirmed
    meshtastic_Config cfg = meshtastic_Config_init_zero;
    cfg.which_payload_variant = meshtastic_Config_lora_tag;
    cfg.payload_variant.lora = mesh_edit.lora;
    mesh_db_set_config(&cfg);
    strcpy(mesh_db.owner.long_name, "Calc 12Tc");
    node_back();
    CHECK(mesh_edit_state() == MESH_EDIT_DONE);
    CHECK(mesh_edit_dirty() == 0);
    CHECK(mesh_edit_base.lora.region == meshtastic_Config_LoRaConfig_RegionCode_EU_433);
    press(KBD_UP);
    CHECK(mesh_edit_state() == MESH_EDIT_DONE); // still showing
    press(KBD_HOME);
    downs(3); // role
    press(KBD_RIGHT); // the next change clears it
    CHECK(mesh_edit_state() == MESH_EDIT_IDLE);
    press(KBD_LEFT);

    // Comes back without it: flagged
    press(KBD_HOME);
    downs(7);
    press(KBD_RIGHT); // hop limit 3 -> 4
    CHECK(mesh_edit.lora.hop_limit == 4);
    press(KBD_HOME);
    downs(19);
    press(KBD_ENTER);
    press('y');
    node_back();
    CHECK(mesh_edit_state() == MESH_EDIT_MISMATCH);
    CHECK(mesh_edit_mismatch() == MESH_EDIT_LORA);
    CHECK(mesh_edit.lora.hop_limit == 3);

    // Node not there: nothing changes, nothing is sent
    fake_admin_sent = 0;
    fake_link_state = MESH_LINK_PASSIVE;
    press(KBD_HOME);
    downs(7);
    press(KBD_RIGHT);
    CHECK(mesh_edit.lora.hop_limit == 3);
    CHECK(!mesh_edit_write());
    ui_render();
    fake_link_state = MESH_LINK_READY;

    // calcmesh's own: change at once, and get saved
    press(KBD_HOME);
    downs(19); // past Bluetooth: no Write row now
    int lcd = settings_get_int(SET_SCREEN_BACKLIGHT, -1);
    press(KBD_RIGHT);
    CHECK(settings_get_int(SET_SCREEN_BACKLIGHT, -1) == lcd + 16);
    CHECK(settings_dirty());
    for (int i = 0; i < 30; i++)
        press(KBD_RIGHT);
    CHECK(settings_get_int(SET_SCREEN_BACKLIGHT, -1) == 240);
    // Set off a step by the keyboard's own keys: back onto one
    settings_set_int(SET_SCREEN_BACKLIGHT, 100);
    press(KBD_LEFT);
    CHECK(settings_get_int(SET_SCREEN_BACKLIGHT, -1) == 96);
    press(KBD_DOWN);
    settings_set_int(SET_KEYS_BACKLIGHT, 250); // the keyboard says off
    press(KBD_RIGHT);
    CHECK(settings_get_int(SET_KEYS_BACKLIGHT, -1) == 32);
    press(KBD_DOWN);
    press(KBD_LEFT);
    CHECK(strcmp(settings_get(SET_DESKTOP_MODE), "off") == 0);
    press(KBD_DOWN); // timeout: minutes near 10, coarser further out
    press(KBD_RIGHT);
    CHECK(settings_get_int(SET_DESKTOP_IDLE, -1) == 12);
    for (int i = 0; i < 20; i++)
        press(KBD_RIGHT);
    CHECK(settings_get_int(SET_DESKTOP_IDLE, -1) == 120);
    settings_set_int(SET_DESKTOP_IDLE, 25); // from the console: between steps
    press(KBD_LEFT);
    CHECK(settings_get_int(SET_DESKTOP_IDLE, -1) == 20);
    for (int i = 0; i < 20; i++)
        press(KBD_LEFT);
    CHECK(settings_get_int(SET_DESKTOP_IDLE, -1) == 6);
    for (int i = 0; i < 40; i++)
        press(KBD_PGDN);
    press(KBD_ENTER); // key echo
    CHECK(settings_get_bool(SET_KEY_ECHO, false));
    ui_render();
}

// Let the fake remote node answer until the job is done (or 200 rounds)
static void remote_run(void) {
    for (int i = 0; i < 200 && ra_state() == RA_BUSY; i++) {
        if (!fake_remote_answer())
            fake_ms += 1000; // waiting (for the restart)
        mesh_edit_poll();
        ui_render();
    }
}

static void test_remote_tab(void) {
    ui_init();
    scene_load();
    mesh_edit_poll();
    press(KBD_F1 + 4);
    press(KBD_HOME); // whose settings
    press(KBD_ENTER);
    ui_render();
    // Ours, then favorites by last heard (Ridge 40s, Hilltop 10m, Library
    // 40m), then the rest; Enter picks
    press(KBD_DOWN);
    fake_remote_sent = 0;
    press(KBD_F1); // the picker holds the keys
    CHECK(ui_tab() == UI_TAB_SETTINGS);
    press(KBD_ENTER);
    CHECK(mesh_edit_target() == scene_node(0));
    CHECK(mesh_edit_available() == 0);
    CHECK(fake_remote_sent == 0); // nothing on air until asked
    ui_render();

    // Enter again (it's on Fetch now): asks, a message at a time
    press(KBD_ENTER);
    CHECK(ra_state() == RA_BUSY);
    ui_render();
    remote_run();
    CHECK(ra_state() == RA_DONE);
    CHECK(strcmp(mesh_edit_base.owner.long_name, "Ridge Repeater") == 0);
    CHECK(mesh_edit_base.lora.tx_power == 27);
    CHECK(fake_remote_sent == 10); // metadata, owner, 5 configs, channels 0-2

    // Hop limit: whose, long, short, role, NodeInfo, region, preset, hops
    press(KBD_HOME);
    downs(7);
    press(KBD_RIGHT);
    CHECK(mesh_edit.lora.hop_limit == 5);
    // tx power, transmit, 2 channels, gps, interval, smart, fixed, power, bt
    downs(11);
    press(KBD_ENTER);
    ui_render();
    press('y');
    CHECK(mesh_edit_state() == MESH_EDIT_RESTARTING);
    press(KBD_HOME);
    press(KBD_ENTER); // can't switch nodes mid-write: no picker
    press(KBD_F1);
    CHECK(ui_tab() == UI_TAB_NODES);
    press(KBD_F1 + 4);
    remote_run();
    CHECK(mesh_edit_state() == MESH_EDIT_DONE);
    CHECK(fake_remote.lora.hop_limit == 5 && mesh_edit_base.lora.hop_limit == 5);
    ui_render();

    // Refused: the remote node doesn't have our key
    fake_remote_error = meshtastic_Routing_Error_ADMIN_PUBLIC_KEY_UNAUTHORIZED;
    press(KBD_HOME);
    downs(7);
    press(KBD_RIGHT);
    downs(11);
    press(KBD_ENTER);
    press('y');
    remote_run();
    CHECK(mesh_edit_state() == MESH_EDIT_REFUSED);
    CHECK(mesh_edit.lora.hop_limit == 6); // kept, to try again
    ui_render();
    fake_remote_error = 0;

    // Back to our own node
    press(KBD_HOME);
    press(KBD_ENTER);
    press(KBD_HOME);
    press(KBD_ENTER);
    CHECK(mesh_edit_target() == 0);
    CHECK(strcmp(mesh_edit_base.owner.long_name, "PicoCalc") == 0);
    ui_render();
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
    test_settings_tab();
    test_remote_tab();
    test_stress();
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    else
        printf("ok\n");
    return failures != 0;
}
