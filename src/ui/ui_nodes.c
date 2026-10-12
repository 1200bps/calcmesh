// Nodes tab: everyone in mesh_db, our own node first, then favourites, then
// most recently heard. Enter opens a DM with the selected node; i asks it for
// its NodeInfo. Nodes our node no longer lists (kept from earlier, or from the
// SD card) are drawn grey.

#include <stdio.h>
#include <stdlib.h>

#include "link.h"
#include "ui.h"
#include "ui_internal.h"

#define ROWS ((UI_BODY_H - UI_LH - 2) / UI_LH)

// Columns, in cells
#define C_FAV 0
#define C_SHORT 1
#define C_LONG 6
#define W_LONG 23
#define C_HOPS 30
#define C_SNR 34
#define C_HEARD 41
#define C_BATT 47 // ends a cell short of the edge, clear of the scroll bar

static ui_list_t list;
static uint32_t sel_num; // keeps the selection on the same node across resorts
static uint16_t order[MESH_MAX_NODES];
static char notice[64]; // until the next key

static int rank(const mesh_node_t *n) {
    if (n->num == mesh_db.my_node_num)
        return 0;
    return n->is_favorite ? 1 : 2;
}

static int cmp(const void *a, const void *b) {
    const mesh_node_t *x = &mesh_db.nodes[*(const uint16_t *)a];
    const mesh_node_t *y = &mesh_db.nodes[*(const uint16_t *)b];
    if (rank(x) != rank(y))
        return rank(x) - rank(y);
    if (x->last_heard != y->last_heard)
        return x->last_heard > y->last_heard ? -1 : 1;
    return x->num < y->num ? -1 : x->num > y->num;
}

static int sort(void) {
    int n = mesh_db.node_count;
    for (int i = 0; i < n; i++)
        order[i] = (uint16_t)i;
    qsort(order, n, sizeof order[0], cmp);
    // Follow the selected node to wherever it sorted to
    for (int i = 0; i < n; i++)
        if (mesh_db.nodes[order[i]].num == sel_num) {
            list.sel = i;
            break;
        }
    ui_list_fix(&list, n, ROWS);
    if (n)
        sel_num = mesh_db.nodes[order[list.sel]].num;
    return n;
}

static void row(int y, const mesh_node_t *n, bool selected) {
    uint8_t bg = selected ? FB_WHITE : FB_CLEAR;
    uint8_t fg = selected ? FB_BLACK : FB_WHITE;
    uint8_t dim = selected ? FB_BLACK : FB_GREY;
    bool me = n->num == mesh_db.my_node_num;
    if (n->cached && !selected)
        fg = FB_GREY;
    char buf[16];

    if (selected)
        fb_fill(0, y, FB_W, UI_LH, FB_WHITE);
    if (n->is_favorite)
        ui_cells(C_FAV, y, 1, "*", fg, bg);
    ui_cells(C_SHORT, y, 4, ui_node_short(n->num), fg, FB_CLEAR);
    ui_cells(C_LONG, y, W_LONG, ui_node_long(n->num), fg, FB_CLEAR);

    if (me)
        ui_cells_right(C_HOPS, y, 3, "me", dim, FB_CLEAR);
    else if (n->hops_away != MESH_HOPS_UNKNOWN) {
        snprintf(buf, sizeof buf, "%u", n->hops_away);
        ui_cells_right(C_HOPS, y, 3, buf, fg, FB_CLEAR);
    } else
        ui_cells_right(C_HOPS, y, 3, "?", dim, FB_CLEAR);

    // SNR only means something for nodes we hear directly
    if (!me && n->hops_away == 0 && n->last_heard) {
        snprintf(buf, sizeof buf, "%.1f", (double)n->snr);
        ui_cells_right(C_SNR, y, 6, buf, fg, FB_CLEAR);
    }
    if (!me)
        ui_cells_right(C_HEARD, y, 5, ui_ago(n->last_heard, buf), fg, FB_CLEAR);

    if (n->battery == MESH_BATTERY_POWERED)
        ui_cells_right(C_BATT, y, 5, "pwr", dim, FB_CLEAR);
    else if (n->battery != MESH_BATTERY_UNKNOWN) {
        snprintf(buf, sizeof buf, "%u%%", n->battery);
        ui_cells_right(C_BATT, y, 5, buf, n->battery < 20 && !selected ? FB_WHITE : dim,
                       FB_CLEAR);
    }
}

void ui_nodes_draw(void) {
    int n = sort();
    int y = UI_BODY_Y + 1;

    ui_cells(C_SHORT, y, 4, "Name", FB_DIM, FB_CLEAR);
    ui_cells_right(C_HOPS, y, 3, "Hop", FB_DIM, FB_CLEAR);
    ui_cells_right(C_SNR, y, 6, "SNR", FB_DIM, FB_CLEAR);
    ui_cells_right(C_HEARD, y, 5, "Heard", FB_DIM, FB_CLEAR);
    ui_cells_right(C_BATT, y, 5, "Bat", FB_DIM, FB_CLEAR);
    y += UI_LH + 1;
    fb_hline(0, y - 1, FB_W, FB_DIM);

    if (!n) {
        ui_cells(1, y + UI_LH, UI_COLS - 2, "No nodes yet.", FB_GREY, FB_CLEAR);
        return;
    }
    for (int i = list.top; i < n && i < list.top + ROWS; i++, y += UI_LH)
        row(y, &mesh_db.nodes[order[i]], i == list.sel);

    // Scroll bar when the list doesn't fit
    if (n > ROWS) {
        int top = UI_BODY_Y + UI_LH + 2, h = ROWS * UI_LH;
        int bh = h * ROWS / n, by = top + h * list.top / n;
        fb_vline(FB_W - 2, top, h, FB_DIM);
        fb_fill(FB_W - 3, by, 3, bh < 4 ? 4 : bh, FB_WHITE);
    }

    if (notice[0]) {
        int ny = UI_BODY_Y + UI_BODY_H - UI_LH;
        fb_fill(0, ny - 2, FB_W, UI_LH + 2, FB_BLACK);
        fb_hline(0, ny - 2, FB_W, FB_DIM);
        ui_cells(1, ny, UI_COLS - 2, notice, FB_WHITE, FB_CLEAR);
    }
}

// i: ask the selected node for its NodeInfo (name and key; without its
// key our node won't send it DMs or metrics requests)
static void ask_nodeinfo(void) {
    const mesh_node_t *n = mesh_db_find(sel_num);
    if (!n || sel_num == mesh_db.my_node_num)
        return;
    if (mesh_link_send_nodeinfo_req(sel_num))
        snprintf(notice, sizeof notice, "Asked %s for its NodeInfo%s", ui_node_short(sel_num),
                 n->has_key ? "." : "; no key for it yet.");
    else
        snprintf(notice, sizeof notice, "Can't ask: not connected to our node.");
}

bool ui_nodes_key(const kbd_event_t *ev) {
    bool had_notice = notice[0];
    notice[0] = '\0';
    int n = sort();
    if (!n)
        return had_notice;
    if (ui_list_key(&list, n, ROWS, ev->key)) {
        sel_num = mesh_db.nodes[order[list.sel]].num;
        return true;
    }
    if (ev->key == KBD_ENTER && sel_num != mesh_db.my_node_num) {
        ui_chats_open(MESH_CONV_DM(sel_num));
        return true;
    }
    if (ev->key == 'i' || ev->key == 'I') {
        ask_nodeinfo();
        return true;
    }
    return had_notice;
}
