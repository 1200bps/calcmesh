// Traceroute page, over the Nodes tab: the route out to a node and back,
// one node per row along a line, with the SNR each was heard at. t traces
// again (once the 30 s our node insists on have passed); Esc goes back.

#include <stdio.h>

#include "link.h"
#include "trace.h"
#include "ui_internal.h"

// Columns, in cells, from the start of each half
#define H_OUT 1
#define H_BACK 27
#define C_MARK 0 // the line and node squares
#define C_SHORT 2
#define C_SNR 7
#define W_SNR 6
#define C_LONG 14
#define W_LONG 11

static uint32_t node; // 0: closed
static char notice[64];

bool ui_trace_active(void) { return node != 0; }

static void send(void) {
    switch (mesh_trace_send(node)) {
    case TRACE_SENT:
        notice[0] = '\0';
        break;
    case TRACE_TOO_SOON: // the footer counts down
        break;
    case TRACE_NO_LINK:
        snprintf(notice, sizeof notice, "Can't trace: not connected to our node.");
        break;
    case TRACE_BAD_NODE:
        break;
    }
}

void ui_trace_open(uint32_t num) {
    node = num;
    notice[0] = '\0';
    // A trace still under way, or wait for the gap: show what there is
    const mesh_trace_t *t = mesh_trace_find(num);
    if (t && t->state == TRACE_WAITING)
        return;
    if (mesh_trace_wait_ms() && t)
        return;
    send();
}

// One end or relay: a filled square for the ends, hollow for relays
static void hop(int base, int y, uint32_t num, int8_t snr, bool end, bool first) {
    int x = (base + C_MARK) * UI_CW + 1;
    if (end)
        fb_fill(x, y + 3, 6, 6, FB_WHITE);
    else {
        fb_fill(x, y + 3, 6, 6, FB_BLACK);
        fb_rect(x, y + 3, 6, 6, FB_WHITE);
    }
    bool unknown = num == MESH_TRACE_UNKNOWN;
    uint8_t fg = unknown ? FB_GREY : FB_WHITE;
    ui_cells(base + C_SHORT, y, 4, unknown ? "?" : ui_node_short(num), fg, FB_CLEAR);
    if (!first) {
        char buf[12];
        if (snr == MESH_TRACE_SNR_UNKNOWN)
            ui_cells_right(base + C_SNR, y, W_SNR, "?", FB_GREY, FB_CLEAR);
        else {
            snprintf(buf, sizeof buf, "%.2f", snr / 4.0);
            ui_cells_right(base + C_SNR, y, W_SNR, buf, fg, FB_CLEAR);
        }
    }
    const char *name = unknown ? "unknown" : num == mesh_db.my_node_num ? "us"
                                                                          : ui_node_long(num);
    ui_cells(base + C_LONG, y, W_LONG, name, FB_GREY, FB_CLEAR);
}

// from, the relays, to: the line first, then the squares over it
static void route(int base, int y, uint32_t from, const uint32_t *relays, int n,
                  const int8_t *snr, uint32_t to) {
    int x = (base + C_MARK) * UI_CW + 4;
    fb_vline(x, y + 6, (n + 1) * UI_LH, FB_GREY);
    hop(base, y, from, 0, true, true);
    for (int i = 0; i < n; i++)
        hop(base, y + (i + 1) * UI_LH, relays[i], snr[i], false, false);
    hop(base, y + (n + 1) * UI_LH, to, snr[n], true, false);
}

// "Out    dB    3 hops" over one half
static void heading(int base, int y, const char *name, int relays, bool known) {
    ui_cells(base + C_SHORT, y, 5, name, FB_DIM, FB_CLEAR);
    if (!known)
        return;
    char buf[20];
    ui_cells_right(base + C_SNR, y, W_SNR, "dB", FB_DIM, FB_CLEAR);
    snprintf(buf, sizeof buf, "%d hop%s", relays + 1, relays ? "s" : "");
    ui_cells_right(base + C_LONG, y, W_LONG, buf, FB_DIM, FB_CLEAR);
}

void ui_trace_draw(void) {
    int y = UI_BODY_Y + 1;
    char buf[64];
    ui_cells(0, y, 11, "Traceroute", FB_DIM, FB_CLEAR);
    ui_cells(11, y, 5, ui_node_short(node), FB_WHITE, FB_CLEAR);
    ui_cells(16, y, UI_COLS - 16, ui_node_long(node), FB_WHITE, FB_CLEAR);
    y += UI_LH;

    const mesh_trace_t *t = mesh_trace_find(node);
    uint32_t now = mesh_link_ms();
    if (!t)
        snprintf(buf, sizeof buf, mesh_trace_wait_ms()
                                      ? "Not traced yet: our node takes one per 30 s."
                                      : "Not traced yet.");
    else if (t->state == TRACE_WAITING)
        snprintf(buf, sizeof buf, "Waiting for the answer: %lus of %lus.",
                 (unsigned long)((now - t->sent_ms) / 1000), (unsigned long)(t->wait_ms / 1000));
    else if (t->state == TRACE_FAILED && t->error == TRACE_ERR_NO_ANSWER)
        snprintf(buf, sizeof buf, "No answer in %lus.", (unsigned long)(t->wait_ms / 1000));
    else if (t->state == TRACE_FAILED)
        snprintf(buf, sizeof buf, "Failed: %s",
                 t->error == TRACE_ERR_REFUSED ? t->note : mesh_routing_error_name(t->error));
    else {
        char ago[8];
        uint32_t secs = (now - t->sent_ms) / 1000;
        if (secs < 60)
            snprintf(ago, sizeof ago, "%lus", (unsigned long)secs);
        else if (secs < 3600)
            snprintf(ago, sizeof ago, "%lum", (unsigned long)(secs / 60));
        else
            snprintf(ago, sizeof ago, "%luh", (unsigned long)(secs / 3600));
        snprintf(buf, sizeof buf, "Answered in %.1fs, %s ago.", t->took_ms / 1000.0, ago);
    }
    ui_cells(0, y, UI_COLS, buf, t && t->state == TRACE_DONE ? FB_GREY : FB_WHITE, FB_CLEAR);
    y += UI_LH + 4;

    if (t && t->state == TRACE_DONE) {
        uint32_t me = mesh_db.my_node_num;
        heading(H_OUT, y, "Out", t->n_out, true);
        heading(H_BACK, y, "Back", t->n_back, t->back_known);
        y += UI_LH + 1;
        fb_hline(0, y - 1, FB_W, FB_DIM);
        y += 2;
        route(H_OUT, y, me, t->out, t->n_out, t->snr_out, node);
        if (t->back_known)
            route(H_BACK, y, node, t->back, t->n_back, t->snr_back, me);
        else
            ui_cells(H_BACK, y, 25, "not reported", FB_GREY, FB_CLEAR);
    }

    // Keys, or why t didn't go
    int ny = UI_BODY_Y + UI_BODY_H - UI_LH;
    fb_hline(0, ny - 2, FB_W, FB_DIM);
    if (notice[0])
        ui_cells(1, ny, UI_COLS - 2, notice, FB_WHITE, FB_CLEAR);
    else {
        uint32_t wait = mesh_trace_wait_ms();
        if (t && t->state == TRACE_WAITING)
            snprintf(buf, sizeof buf, "Esc back");
        else if (wait)
            snprintf(buf, sizeof buf, "t trace%s in %lus   Esc back", t ? " again" : "",
                     (unsigned long)((wait + 999) / 1000));
        else
            snprintf(buf, sizeof buf, "t trace%s   Esc back", t ? " again" : "");
        ui_cells(1, ny, UI_COLS - 2, buf, FB_GREY, FB_CLEAR);
    }
}

bool ui_trace_key(const kbd_event_t *ev) {
    bool had_notice = notice[0];
    notice[0] = '\0';
    switch (ev->key) {
    case KBD_ESC:
    case KBD_BACKSPACE:
    case KBD_LEFT:
        node = 0;
        return true;
    case 't':
    case 'T': {
        const mesh_trace_t *t = mesh_trace_find(node);
        if (!t || t->state != TRACE_WAITING)
            send();
        return true;
    }
    default:
        return had_notice;
    }
}
