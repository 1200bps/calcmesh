// Render the UI on the host, against a made-up mesh (fake/scene.c), and save
// the screen as PNG.
//
//   uishot [-s SCALE] OUT.png [KEY...]   press KEYs, then save one screen
//   uishot [-s SCALE] -t DIR             save a tour of the main screens
//
// KEY is a key name (tab s-tab enter esc up down left right home end pgup
// pgdn bs del f1..f10) or =TEXT to type TEXT.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "msgs.h"
#include "fb.h"
#include "nodedb.h"
#include "mesh/nodeedit.h"
#include "meshtastic/mesh.pb.h"
#include "png.h"
#include "metsched.h"
#include "ff.h"
#include "ramdisk.h"
#include "sd/metlog.h"
#include "sd/metview.h"
#include "scene.h"
#include "ui.h"

static int scale = 2;

static void press(uint16_t key, uint8_t mods) {
    kbd_event_t ev = {.key = key, .mods = mods};
    ui_key(&ev);
}

static void type(const char *s) {
    while (*s)
        press((uint8_t)*s++, 0);
}

static bool key(const char *name) {
    static const struct {
        const char *name;
        uint16_t key;
    } keys[] = {
        {"tab", KBD_TAB},     {"enter", KBD_ENTER}, {"esc", KBD_ESC},
        {"up", KBD_UP},       {"down", KBD_DOWN},   {"left", KBD_LEFT},
        {"right", KBD_RIGHT}, {"home", KBD_HOME},   {"end", KBD_END},
        {"pgup", KBD_PGUP},   {"pgdn", KBD_PGDN},   {"bs", KBD_BACKSPACE},
        {"del", KBD_DEL},
    };
    if (name[0] == '=') {
        type(name + 1);
        return true;
    }
    if (strcasecmp(name, "s-tab") == 0) {
        press(KBD_TAB, KBD_MOD_SHIFT);
        return true;
    }
    if ((name[0] == 'f' || name[0] == 'F') && name[1] >= '1' && name[1] <= '9') {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 10) {
            press((uint16_t)(KBD_F1 + n - 1), 0);
            return true;
        }
    }
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        if (strcasecmp(name, keys[i].name) == 0) {
            press(keys[i].key, 0);
            return true;
        }
    return false;
}

static int shot(const char *dir, const char *name) {
    char path[512];
    if (dir)
        snprintf(path, sizeof path, "%s/%s.png", dir, name);
    else
        snprintf(path, sizeof path, "%s", name);
    // Anything the screen wants from the card, then the screen again
    ui_render();
    while (metview_pending())
        metview_task();
    ui_render();
    if (!png_write_fb(path, scale)) {
        perror(path);
        return 1;
    }
    printf("%s\n", path);
    return 0;
}

// A card with a week of our node's metrics in the log, as metlog.c writes
// it: the 18650 run down and charged, the channel busy in the evenings, a
// night switched off.
static uint32_t rnd_state = 12345;
static float rnd(void) {
    rnd_state = rnd_state * 1103515245u + 12345u;
    return (float)(rnd_state >> 16 & 0x7fff) / 32768.0f;
}

static void card_week(void) {
    static FATFS fs;
    static BYTE work[FF_MAX_SS];
    ramdisk_create(8u << 20);
    MKFS_PARM opt = {.fmt = FM_FAT | FM_SFD};
    if (f_mkfs("", &opt, work, sizeof work) != FR_OK || f_mount(&fs, "", 1) != FR_OK)
        return;
    metlog_set_online(true);
    metview_set_online(true);
    mesh_metrics_t e;
    memset(&e, 0, sizeof e);
    e.num = SCENE_ME;
    float mv = 3.95f;
    uint32_t up = 0, rx = 0;
    for (uint32_t t = SCENE_NOW - 7 * 86400; t < SCENE_NOW - 8 * 3600; t += 600) {
        uint32_t day = (SCENE_NOW - t) / 86400, hour = t % 86400 / 3600;
        if (day == 4 && (hour >= 23 || hour < 7)) { // off overnight
            up = rx = 0;
            continue;
        }
        // Down 4 mV an hour on, charged up on the fifth day back and yesterday
        bool charging = (day == 5 || day == 1) && hour >= 9 && hour < 13;
        mv = charging ? mv + 0.03f : mv - 0.0006f;
        mv = mv > 4.18f ? 4.18f : mv < 3.4f ? 3.4f : mv;
        float eve = hour >= 17 && hour <= 23 ? 6 : 0;
        meshtastic_DeviceMetrics *d = &e.device;
        memset(d, 0, sizeof *d);
        d->has_voltage = d->has_battery_level = d->has_channel_utilization = true;
        d->has_air_util_tx = d->has_uptime_seconds = true;
        d->voltage = mv + 0.01f * rnd();
        d->battery_level = (uint32_t)((mv - 3.4f) / 0.78f * 100);
        d->channel_utilization = 8 + eve + 4 * rnd() + (day == 1 ? 7 : 0); // tropo
        d->air_util_tx = 0.6f + 0.5f * rnd();
        d->uptime_seconds = up;
        metlog_telemetry(&e, MET_DEVICE, t, false);
        if (t % 3600 < 600) {
            meshtastic_LocalStats *l = &e.local;
            memset(l, 0, sizeof *l);
            rx += (uint32_t)(60 + 8 * eve + 40 * rnd() + (day == 1 ? 120 : 0));
            l->uptime_seconds = up;
            l->num_packets_rx = rx;
            l->num_online_nodes = (uint32_t)(12 + eve + 4 * rnd() + (day == 1 ? 14 : 0));
            l->num_total_nodes = 40;
            l->noise_floor = (int32_t)(-112 + 4 * rnd());
            l->channel_utilization = d->channel_utilization;
            metlog_telemetry(&e, MET_LOCAL, t, false);
        }
        while (metlog_pending())
            metlog_flush();
        up += 600;
    }
}

static int tour(const char *dir) {
    int err = 0;
    err |= shot(dir, "1-nodes");
    key("down"), key("down"), key("down");
    err |= shot(dir, "2-nodes-selected");
    key("tab");
    err |= shot(dir, "3-chats");
    key("enter");
    err |= shot(dir, "4-chat-longfast");
    type("Testing from the PicoCalc, keyboard and all.");
    for (int i = 0; i < 9; i++)
        key("left");
    err |= shot(dir, "5-chat-compose");
    key("esc"), key("down"), key("down"), key("down"), key("down"), key("enter");
    err |= shot(dir, "6-chat-dm");
    key("f3");
    err |= shot(dir, "19-position");
    type("]]]");
    err |= shot(dir, "20-position-selected");
    type("++");
    err |= shot(dir, "21-position-zoomed");
    type("0[[[[");
    err |= shot(dir, "22-position-far");
    key("esc");
    key("f1");
    // The status line in its other states, on the Nodes tab
    fake_link_state = MESH_LINK_PASSIVE;
    ui_set_battery(87, true);
    err |= shot(dir, "7-status-desktop");
    fake_link_state = MESH_LINK_CONFIGURING;
    ui_set_battery(12, false);
    err |= shot(dir, "8-status-syncing");
    fake_link_state = MESH_LINK_READY;
    ui_set_battery(0, false);
    err |= shot(dir, "9-status-usb");
    ui_set_battery(64, false);
    fake_link_state = MESH_LINK_WAKING;
    fake_node_silent = true;
    mesh_db_clear();
    mesh_msgs_clear();
    key("f1");
    err |= shot(dir, "10-no-node");

    // Settings
    scene_load();
    key("f5");
    err |= shot(dir, "11-settings");
    for (int i = 0; i < 4; i++)
        key("down");
    key("right"), key("right");
    key("down"), key("down"), key("right");
    key("home"), key("enter");
    for (int i = 0; i < 8; i++)
        key("bs");
    type("Calc 12T");
    err |= shot(dir, "12-settings-edit");
    type("c");
    key("enter");
    for (int i = 0; i < 18; i++)
        key("down");
    err |= shot(dir, "13-settings-changed");
    key("enter");
    err |= shot(dir, "14-settings-confirm");
    type("y");
    err |= shot(dir, "15-settings-writing");
    for (int i = 0; i < 40; i++)
        key("pgdn");
    err |= shot(dir, "16-settings-calcmesh");
    // The node comes back with only some of it
    key("home");
    strcpy(mesh_db.owner.long_name, "Calc 12Tc");
    fake_link_stats.syncs++;
    mesh_edit_poll();
    err |= shot(dir, "17-settings-result");
    mesh_edit_ack();
    // Reboot: the last of our node's rows, seven above the last one
    key("end");
    for (int i = 0; i < 7; i++)
        key("up");
    err |= shot(dir, "45-settings-reboot");
    key("enter");
    err |= shot(dir, "46-settings-reboot-confirm");
    type("y");
    err |= shot(dir, "47-settings-rebooting");
    fake_link_stats.syncs++;
    mesh_edit_poll();
    mesh_edit_ack();
    fake_link_state = MESH_LINK_WAKING;
    fake_node_silent = true;
    mesh_db_clear();
    mesh_edit_load();
    err |= shot(dir, "18-settings-no-node");

    key("f3");
    err |= shot(dir, "23-position-empty");

    // Another node's settings, over the mesh (24 on: Position has 19-23)
    fake_link_state = MESH_LINK_READY;
    fake_node_silent = false;
    scene_load();
    key("f1"), key("f5"), key("home"), key("enter");
    err |= shot(dir, "24-remote-pick");
    key("down"), key("enter");
    err |= shot(dir, "25-remote-unfetched");
    key("enter");
    for (int i = 0; i < 3; i++)
        fake_remote_answer(), mesh_edit_poll();
    err |= shot(dir, "26-remote-fetching");
    while (fake_remote_answer())
        mesh_edit_poll();
    err |= shot(dir, "27-remote-fetched");
    key("home");
    for (int i = 0; i < 7; i++)
        key("down");
    key("right");
    for (int i = 0; i < 11; i++)
        key("down");
    key("enter");
    err |= shot(dir, "28-remote-confirm");
    type("y");
    for (int i = 0; i < 3; i++)
        fake_remote_answer(), mesh_edit_poll();
    err |= shot(dir, "29-remote-writing");
    for (int i = 0; i < 60 && mesh_edit_state() == MESH_EDIT_RESTARTING; i++) {
        if (!fake_remote_answer())
            fake_ms += 1000;
        mesh_edit_poll();
    }
    key("home");
    err |= shot(dir, "30-remote-done");
    // One it refuses
    fake_remote_error = meshtastic_Routing_Error_ADMIN_PUBLIC_KEY_UNAUTHORIZED;
    for (int i = 0; i < 7; i++)
        key("down");
    key("right");
    for (int i = 0; i < 11; i++)
        key("down");
    key("enter");
    type("y");
    fake_remote_answer();
    mesh_edit_poll();
    key("home");
    err |= shot(dir, "31-remote-refused");
    fake_remote_error = 0;

    // Metrics (32 on)
    scene_load();
    key("f4");
    err |= shot(dir, "32-metrics");
    key("enter");
    err |= shot(dir, "33-metrics-ours");
    key("end");
    err |= shot(dir, "34-metrics-ours-local");
    key("esc"), key("down"), key("down"), key("enter");
    err |= shot(dir, "35-metrics-ridge");
    key("pgdn");
    err |= shot(dir, "36-metrics-ridge-more");
    type("l");
    fake_ms += 12000;
    err |= shot(dir, "37-metrics-asking");
    scene_telemetry_answer(0);
    key("end");
    err |= shot(dir, "38-metrics-answered");
    key("esc"), key("home"), key("enter");
    key("right"), key("right");
    type("p");
    scene_telemetry_answer(0);
    err |= shot(dir, "39-metrics-no-sensor");
    key("esc"), key("home");
    for (int i = 0; i < 7; i++)
        key("down");
    key("enter");
    err |= shot(dir, "40-metrics-weather");

    // Scheduling Ridge Repeater: device and local stats every 2 hours
    fake_epoch = SCENE_NOW;
    key("esc"), key("home"), key("down"), key("enter");
    type("s");
    key("right");
    err |= shot(dir, "41-metrics-sched-edit");
    key("enter");
    metsched_task(); // runs at once
    fake_ms += 9000;
    key("end");
    err |= shot(dir, "42-metrics-sched-asking");
    scene_telemetry_answer(0);
    // No local stats, nor when asked again
    for (int i = 0; i < (METSCHED_GAP_MS + 2 * METSCHED_WAIT_MS + METSCHED_RETRY_MS) / 500 + 4; i++)
        fake_ms += 500, mesh_metrics_poll(), metsched_task();
    fake_ms += 25 * 60000;
    fake_epoch += 25 * 60;
    ui_set_now(SCENE_NOW + 25 * 60);
    type("x"); // redraw
    key("end");
    err |= shot(dir, "43-metrics-sched");
    key("esc");
    err |= shot(dir, "44-metrics-sched-list");
    metsched_set(scene_node(0), 0, 0);

    // A week of our own node in the log on the card, off for a night
    card_week();
    ui_set_now(SCENE_NOW);
    key("home"), key("enter");
    err |= shot(dir, "45-metrics-history-24h");
    type("]]");
    err |= shot(dir, "46-metrics-history-7d");
    key("end");
    err |= shot(dir, "47-metrics-history-7d-local");
    type("[[[[");
    key("home");
    err |= shot(dir, "48-metrics-history-3h");
    return err;
}

int main(int argc, char **argv) {
    int i = 1;
    if (i + 1 < argc && strcmp(argv[i], "-s") == 0) {
        scale = atoi(argv[i + 1]);
        i += 2;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: uishot [-s SCALE] OUT.png [KEY...]\n"
                        "       uishot [-s SCALE] -t DIR\n");
        return 2;
    }
    ui_init();
    scene_load();
    ui_set_battery(64, false);
    if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
        return tour(argv[i + 1]);

    const char *out = argv[i++];
    for (; i < argc; i++)
        if (!key(argv[i])) {
            fprintf(stderr, "unknown key '%s'\n", argv[i]);
            return 2;
        }
    return shot(NULL, out);
}
