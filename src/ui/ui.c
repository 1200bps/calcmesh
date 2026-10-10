#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "link.h"
#include "ui_internal.h"

// The views, by softkey slot; NULL is an empty slot.
static const struct {
    const char *name;
    void (*draw)(void);
    bool (*key)(const kbd_event_t *ev);
} views[UI_SLOTS] = {
    [UI_TAB_NODES] = {"Nodes", ui_nodes_draw, ui_nodes_key},
    [UI_TAB_CHATS] = {"Chats", ui_chats_draw, ui_chats_key},
    [UI_TAB_SETTINGS] = {"Settings", ui_settings_draw, ui_settings_key},
};

static ui_tab_t tab;
static uint32_t now_set;
static int battery = -1;
static bool charging;

void ui_init(void) {
    tab = UI_TAB_NODES;
    fb_clear(FB_BLACK);
    fb_invalidate();
}

ui_tab_t ui_tab(void) { return tab; }
void ui_switch_tab(int t) {
    if (t >= 0 && t < UI_SLOTS && views[t].draw)
        tab = (ui_tab_t)t;
}

// The next tab that exists, going dir (+1 or -1) from the current one
static void step_tab(int dir) {
    int t = tab;
    do
        t = (t + dir + UI_SLOTS) % UI_SLOTS;
    while (!views[t].draw);
    tab = (ui_tab_t)t;
}
void ui_set_now(uint32_t epoch) { now_set = epoch; }

void ui_set_battery(int percent, bool chg) {
    battery = percent > 100 ? 100 : percent;
    charging = percent >= 0 && chg;
}

uint32_t ui_now(void) {
    if (now_set)
        return now_set;
    uint32_t epoch = mesh_link_epoch();
    if (epoch)
        return epoch;
    uint32_t newest = 0;
    for (int i = 0; i < mesh_db.node_count; i++)
        if (mesh_db.nodes[i].last_heard > newest)
            newest = mesh_db.nodes[i].last_heard;
    return newest;
}

// --- helpers shared by the views -------------------------------------------

const char *ui_node_short(uint32_t num) {
    static char buf[2][5];
    static int which;
    const mesh_node_t *n = mesh_db_find(num);
    if (n && n->has_user && n->short_name[0])
        return n->short_name;
    char *b = buf[which ^= 1];
    snprintf(b, sizeof buf[0], "%04x", (unsigned)(num & 0xFFFF));
    return b;
}

const char *ui_node_long(uint32_t num) {
    static char buf[2][12];
    static int which;
    const mesh_node_t *n = mesh_db_find(num);
    if (n && n->has_user && n->long_name[0])
        return n->long_name;
    char *b = buf[which ^= 1];
    snprintf(b, sizeof buf[0], "!%08x", (unsigned)num);
    return b;
}

const char *ui_ago(uint32_t then, char *buf) {
    uint32_t now = ui_now();
    if (!then) {
        strcpy(buf, "-");
        return buf;
    }
    uint32_t d = now > then ? now - then : 0;
    if (d < 60)
        snprintf(buf, 6, "%us", (unsigned)d);
    else if (d < 3600)
        snprintf(buf, 6, "%um", (unsigned)(d / 60));
    else if (d < 86400)
        snprintf(buf, 6, "%uh", (unsigned)(d / 3600));
    else if (d < 99 * 86400)
        snprintf(buf, 6, "%ud", (unsigned)(d / 86400));
    else
        strcpy(buf, ">99d");
    return buf;
}

void ui_cells(int col, int y, int cells, const char *s, uint8_t fg, uint8_t bg) {
    if (bg != FB_CLEAR)
        fb_fill(col * UI_CW, y, cells * UI_CW, UI_LH, bg);
    fb_text(col * UI_CW, y, s, cells, UI_FONT, fg, FB_CLEAR);
}

void ui_cells_right(int col, int y, int cells, const char *s, uint8_t fg,
                    uint8_t bg) {
    int n = utf8_cells(s);
    if (bg != FB_CLEAR)
        fb_fill(col * UI_CW, y, cells * UI_CW, UI_LH, bg);
    if (n > cells) // keep the tail; for numbers that's the part that matters
        s += utf8_offset(s, n - cells), n = cells;
    fb_text((col + cells - n) * UI_CW, y, s, cells, UI_FONT, fg, FB_CLEAR);
}

void ui_list_fix(ui_list_t *l, int count, int rows) {
    if (l->sel >= count)
        l->sel = count - 1;
    if (l->sel < 0)
        l->sel = 0;
    if (l->top > l->sel)
        l->top = l->sel;
    if (l->sel >= l->top + rows)
        l->top = l->sel - rows + 1;
    if (l->top > count - rows)
        l->top = count - rows;
    if (l->top < 0)
        l->top = 0;
}

bool ui_list_key(ui_list_t *l, int count, int rows, uint16_t key) {
    switch (key) {
    case KBD_UP: l->sel--; break;
    case KBD_DOWN: l->sel++; break;
    case KBD_PGUP: l->sel -= rows - 1; break;
    case KBD_PGDN: l->sel += rows - 1; break;
    case KBD_HOME: l->sel = 0; break;
    case KBD_END: l->sel = count - 1; break;
    default: return false;
    }
    ui_list_fix(l, count, rows);
    return true;
}

// --- frame -------------------------------------------------------------------

// Softkey row: five equal slots, labels in boxes with clipped corners, the
// current tab inverted. Unused slots stay empty.
static void draw_tabs(void) {
    int y = UI_KEYS_Y;

    for (int t = 0; t < UI_SLOTS; t++) {
        const char *name = views[t].name;
        if (!name)
            continue;
        char label[16];
        if (t == UI_TAB_NODES)
            snprintf(label, sizeof label, "%s %u", name, (unsigned)mesh_db.node_count);
        else if (t == UI_TAB_CHATS && ui_chats_unread())
            snprintf(label, sizeof label, "%s %d", name,
                     ui_chats_unread() > 99 ? 99 : ui_chats_unread());
        else if (t == UI_TAB_SETTINGS && ui_settings_pending())
            snprintf(label, sizeof label, "%s *", name);
        else
            snprintf(label, sizeof label, "%s", name);

        int x = t * UI_SLOT_W + 1, w = UI_SLOT_W - 2, h = UI_KEYS_H;
        bool cur = t == (int)tab;
        if (cur)
            fb_fill(x, y, w, h, FB_WHITE);
        else
            fb_rect(x, y, w, h, FB_GREY);
        // Round the corners off
        fb_pixel(x, y, FB_BLACK);
        fb_pixel(x + w - 1, y, FB_BLACK);
        fb_pixel(x, y + h - 1, FB_BLACK);
        fb_pixel(x + w - 1, y + h - 1, FB_BLACK);

        int cells = utf8_cells(label);
        if (cells > w / UI_CW)
            cells = w / UI_CW;
        fb_text(x + (w - cells * UI_CW) / 2, y + 2, label, cells, UI_FONT,
                cur ? FB_BLACK : FB_WHITE, FB_CLEAR);
    }
}

// Battery: "64%" and a cell outline filled to match, "+" while charging.
// Returns the x where it starts. With no cells in, the keyboard reports 0.
static int draw_battery(int right, int y) {
    if (battery < 0)
        return right;
    if (battery == 0 && !charging) {
        fb_text(right - 3 * UI_CW, y, "usb", 3, UI_FONT, FB_GREY, FB_CLEAR);
        return right - 3 * UI_CW;
    }

    // Body 15x8 with a 2x4 nub, centred on the text's x-height
    int bw = 15, bh = 8, bx = right - bw - 2, by = y + 3;
    fb_rect(bx, by, bw, bh, FB_WHITE);
    fb_fill(bx + bw, by + 2, 2, bh - 4, FB_WHITE);
    int fill = (battery * (bw - 4) + 50) / 100;
    if (battery > 0 && fill < 1)
        fill = 1;
    fb_fill(bx + 2, by + 2, fill, bh - 4, charging ? FB_GREY : FB_WHITE);

    char txt[8];
    snprintf(txt, sizeof txt, "%s%d%%", charging ? "+" : "", battery);
    int n = utf8_cells(txt), x = bx - 2 - n * UI_CW;
    bool low = battery <= 15 && !charging;
    if (low)
        fb_fill(x - 1, y, n * UI_CW + 2, UI_LH, FB_WHITE);
    fb_text(x, y, txt, n, UI_FONT, low ? FB_BLACK : FB_WHITE, FB_CLEAR);
    return low ? x - 1 : x;
}

// Link to the node: a square (solid when connected, hollow while syncing,
// crossed with no node) and a word; desktop mode is an inverted tag, since
// the PC has the node and nothing can be sent from here.
static int draw_link(int right, int y) {
    mesh_link_conn_t c = mesh_link_conn();
    if (c == MESH_CONN_DESKTOP) {
        const char *s = "DESKTOP";
        int w = utf8_cells(s) * UI_CW + 4, x = right - w;
        fb_fill(x, y, w, UI_LH, FB_WHITE);
        fb_text(x + 2, y, s, -1, UI_FONT, FB_BLACK, FB_CLEAR);
        return x;
    }

    static const char *const words[] = {
        [MESH_CONN_NONE] = "no node",
        [MESH_CONN_SYNCING] = "syncing",
        [MESH_CONN_UP] = "connected",
    };
    const char *s = (unsigned)c < sizeof words / sizeof words[0] ? words[c] : "?";
    uint8_t fg = c == MESH_CONN_UP ? FB_GREY : FB_WHITE;
    int n = utf8_cells(s), x = right - n * UI_CW;
    fb_text(x, y, s, n, UI_FONT, fg, FB_CLEAR);

    int sq = 7, sx = x - 3 - sq, sy = y + 3;
    if (c == MESH_CONN_UP) {
        fb_fill(sx, sy, sq, sq, fg);
    } else {
        fb_rect(sx, sy, sq, sq, fg);
        if (c == MESH_CONN_NONE)
            for (int i = 0; i < sq; i++) {
                fb_pixel(sx + i, sy + i, fg);
                fb_pixel(sx + sq - 1 - i, sy + i, fg);
            }
    }
    return sx;
}

// Status line: our node and primary channel on the left; link and the
// PicoCalc's battery on the right.
static void draw_status(void) {
    int y = 1;

    int x = draw_battery(FB_W - 1, y);
    if (x < FB_W - 1)
        x -= 2 * UI_CW;
    x = draw_link(x, y);

    char left[64];
    uint32_t me = mesh_db.my_node_num;
    if (me)
        snprintf(left, sizeof left, "%s  %s", ui_node_short(me),
                 mesh_db_channel_name(0));
    else
        snprintf(left, sizeof left, "calcmesh");
    ui_cells(0, y, x / UI_CW - 1, left, FB_WHITE, FB_CLEAR);

    fb_hline(0, UI_STATUS_RULE, FB_W, FB_DIM);
}

void ui_render(void) {
    fb_unclip();
    fb_clear(FB_BLACK);
    // Body first: showing a conversation marks it read, which the tab's
    // unread count should already reflect
    fb_clip(0, UI_BODY_Y, FB_W, UI_BODY_H);
    views[tab].draw();
    fb_unclip();
    draw_tabs();
    draw_status();
}

void ui_key(const kbd_event_t *ev) {
    // Global: switch tabs. Tab and F1-F5 never mean anything to a view; an
    // F-key over an empty slot does nothing.
    // A view that's taking text (or asking a question) gets Tab first.
    if (views[tab].key == ui_settings_key && ui_settings_modal()) {
        views[tab].key(ev);
        return;
    }
    if (ev->key == KBD_TAB) {
        step_tab((ev->mods & KBD_MOD_SHIFT) ? -1 : 1);
        return;
    }
    if (ev->key >= KBD_F1 && ev->key < KBD_F1 + UI_SLOTS) {
        ui_switch_tab(ev->key - KBD_F1);
        return;
    }
    views[tab].key(ev);
}
