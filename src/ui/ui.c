#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "link.h"
#include "ui_internal.h"

static ui_tab_t tab;
static uint32_t now_set;

void ui_init(void) {
    tab = UI_TAB_NODES;
    fb_clear(FB_BLACK);
    fb_invalidate();
}

ui_tab_t ui_tab(void) { return tab; }
void ui_switch_tab(int t) { tab = (ui_tab_t)((t + UI_TAB_COUNT) % UI_TAB_COUNT); }
void ui_set_now(uint32_t epoch) { now_set = epoch; }

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

static void draw_tabs(void) {
    static const char *const names[UI_TAB_COUNT] = {"Nodes", "Chats"};
    const ui_font_t *f = &font_8x16;
    int x = 4;
    int base = UI_TABS_H - 1;

    for (int t = 0; t < UI_TAB_COUNT; t++) {
        char label[24];
        int unread = t == UI_TAB_CHATS ? ui_chats_unread() : 0;
        if (t == UI_TAB_NODES)
            snprintf(label, sizeof label, "%s %u", names[t],
                     (unsigned)mesh_db.node_count);
        else
            snprintf(label, sizeof label, "%s", names[t]);

        int w = (utf8_cells(label) + 2) * f->w + (unread ? 3 * UI_CW + 4 : 0);
        if (t == (int)tab) {
            // Folder tab: outline open at the bottom, joined to the rule
            fb_hline(x, 1, w, FB_WHITE);
            fb_vline(x, 1, base, FB_WHITE);
            fb_vline(x + w - 1, 1, base, FB_WHITE);
            fb_text(x + f->w, 3, label, -1, f, FB_WHITE, FB_CLEAR);
            fb_hline(0, base, x + 1, FB_WHITE);
            fb_hline(x + w - 1, base, FB_W - (x + w - 1), FB_WHITE);
        } else {
            fb_text(x + f->w, 3, label, -1, f, FB_GREY, FB_CLEAR);
        }
        if (unread) {
            // Unread count as an inverted badge
            char n[12];
            snprintf(n, sizeof n, "%d", unread > 99 ? 99 : unread);
            int bw = utf8_cells(n) * UI_CW + 4;
            int bx = x + w - f->w - bw + 2;
            fb_fill(bx, 5, bw, UI_LH, FB_WHITE);
            fb_text(bx + 2, 5, n, -1, UI_FONT, FB_BLACK, FB_CLEAR);
        }
        x += w + 4;
    }
}

static void draw_status(void) {
    static const char *const states[] = {
        [MESH_LINK_IDLE] = "idle",
        [MESH_LINK_WAKING] = "waking node",
        [MESH_LINK_CONFIGURING] = "loading",
        [MESH_LINK_READY] = "ready",
        [MESH_LINK_PASSIVE] = "desktop mode",
    };
    int y = UI_STATUS_Y;
    fb_hline(0, y, FB_W, FB_DIM);
    y += 2;

    char left[64];
    uint32_t me = mesh_db.my_node_num;
    mesh_link_state_t st = mesh_link_state();
    if (me)
        snprintf(left, sizeof left, "%s  %s", ui_node_short(me),
                 mesh_db_channel_name(0));
    else
        snprintf(left, sizeof left, "no node");
    int col = 0;
    ui_cells(col, y, 30, left, FB_WHITE, FB_CLEAR);

    const char *s = st < sizeof states / sizeof states[0] ? states[st] : "?";
    const mesh_node_t *mine = me ? mesh_db_find(me) : NULL;
    char right[32];
    if (mine && mine->battery == MESH_BATTERY_POWERED)
        snprintf(right, sizeof right, "%s  pwr", s);
    else if (mine && mine->battery != MESH_BATTERY_UNKNOWN)
        snprintf(right, sizeof right, "%s  %u%%", s, mine->battery);
    else
        snprintf(right, sizeof right, "%s", s);
    ui_cells_right(UI_COLS - 22, y, 22, right,
                   st == MESH_LINK_READY ? FB_GREY : FB_WHITE, FB_CLEAR);
}

void ui_render(void) {
    fb_unclip();
    fb_clear(FB_BLACK);
    // Body first: showing a conversation marks it read, which the tab's
    // unread count should already reflect
    fb_clip(0, UI_BODY_Y, FB_W, UI_BODY_H);
    if (tab == UI_TAB_NODES)
        ui_nodes_draw();
    else
        ui_chats_draw();
    fb_unclip();
    draw_tabs();
    draw_status();
}

void ui_key(const kbd_event_t *ev) {
    // Global: switch tabs. Tab and F-keys never mean anything to a view.
    if (ev->key == KBD_TAB) {
        ui_switch_tab(tab + ((ev->mods & KBD_MOD_SHIFT) ? -1 : 1));
        return;
    }
    if (ev->key >= KBD_F1 && ev->key < KBD_F1 + UI_TAB_COUNT) {
        ui_switch_tab(ev->key - KBD_F1);
        return;
    }
    if (tab == UI_TAB_NODES)
        ui_nodes_key(ev);
    else
        ui_chats_key(ev);
}
