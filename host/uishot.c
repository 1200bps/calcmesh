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
#include "png.h"
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
    ui_render();
    if (!png_write_fb(path, scale)) {
        perror(path);
        return 1;
    }
    printf("%s\n", path);
    return 0;
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
    type("nnn");
    err |= shot(dir, "20-position-selected");
    type("++");
    err |= shot(dir, "21-position-zoomed");
    type("0pppp");
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
    fake_link_state = MESH_LINK_WAKING;
    fake_node_silent = true;
    mesh_db_clear();
    mesh_edit_load();
    err |= shot(dir, "18-settings-no-node");

    key("f3");
    err |= shot(dir, "23-position-empty");
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
