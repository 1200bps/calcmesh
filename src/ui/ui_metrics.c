// Metrics tab: telemetry (metrics.h) for every node, and asking a node for
// more.
//
// The list has a row per node: our own first, then favourites, then nodes
// with metrics, freshest first, then the rest. Battery, voltage with a
// sparkline of its history, channel and airtime use, temperature, and how
// long ago the newest metrics came. Enter opens a node.
//
// A node's page has a section per kind of metrics (device, environment,
// power, local stats) with what it last sent, graphs of the history, and a
// key to ask it for a fresh set: D, E, P or L. Asking our own node stays off
// the air; asking another is one packet each way. Up/Down scroll, Left/Right
// step through the list's nodes, Esc goes back to the list.
//
// Below those, the node's metrics schedule (metsched.h). S
// edits it: D E P L pick the kinds, Left/Right how often, Enter saves (none
// picked: no schedule), Esc leaves it as it was. Scheduled nodes have a
// little clock by their name in the list.

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link.h"
#include "metrics.h"
#include "metsched.h"
#include "ui.h"
#include "ui_internal.h"

#include "meshtastic/mesh.pb.h"

// --- list ------------------------------------------------------------------------

#define STR_(x) #x
#define TOSTR(x) STR_(x)

#define HINT_Y (UI_BODY_Y + UI_BODY_H - UI_LH)
#define ROWS ((HINT_Y - 2 - (UI_BODY_Y + UI_LH + 2)) / UI_LH)

// Columns, in cells
#define C_FAV 0
#define C_SHORT 1
#define C_SCHED 5 // a clock: it has a metrics schedule
#define C_BATT 6  // 4
#define C_VOLT 11 // 5
#define C_SPARK 17
#define W_SPARK 10
#define C_CHU 28  // 5
#define C_AIR 34  // 5
#define C_TEMP 40 // 6
#define C_AGE 47  // 5

static ui_list_t list;
static uint32_t sel_num;
static uint16_t order[MESH_MAX_NODES];

static uint32_t newest(const mesh_metrics_t *e) {
    uint32_t t = 0;
    for (int k = 0; k < MET_KINDS; k++)
        if (e->have[k] && e->time[k] > t)
            t = e->time[k];
    return t;
}

static int rank(const mesh_node_t *n, const mesh_metrics_t *e) {
    if (n->num == mesh_db.my_node_num)
        return 0;
    if (n->is_favorite)
        return 1;
    return e ? 2 : 3;
}

static int cmp(const void *a, const void *b) {
    const mesh_node_t *x = &mesh_db.nodes[*(const uint16_t *)a];
    const mesh_node_t *y = &mesh_db.nodes[*(const uint16_t *)b];
    const mesh_metrics_t *ex = mesh_metrics_find(x->num), *ey = mesh_metrics_find(y->num);
    int rx = rank(x, ex), ry = rank(y, ey);
    if (rx != ry)
        return rx - ry;
    uint32_t tx = ex ? newest(ex) : 0, ty = ey ? newest(ey) : 0;
    if (tx != ty)
        return tx > ty ? -1 : 1;
    if (x->last_heard != y->last_heard)
        return x->last_heard > y->last_heard ? -1 : 1;
    return x->num < y->num ? -1 : x->num > y->num;
}

static int sort(void) {
    int n = mesh_db.node_count;
    for (int i = 0; i < n; i++)
        order[i] = (uint16_t)i;
    qsort(order, n, sizeof order[0], cmp);
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

// --- history -------------------------------------------------------------------

typedef enum { S_MV, S_BATT, S_CHUTIL, S_TEMP, S_AIRTX, S_NOISE, S_ONLINE, S_RXPH } series_t;

static float value(const met_point_t *p, series_t s) {
    switch (s) {
    case S_MV: return p->mv ? p->mv / 1000.0f : NAN;
    case S_BATT: return p->batt <= 100 ? p->batt : NAN; // not "powered"
    case S_CHUTIL: return p->chutil != MET_NONE_U8 ? p->chutil / 2.0f : NAN;
    case S_AIRTX: return p->airtx != MET_NONE_U16 ? p->airtx / 100.0f : NAN;
    case S_NOISE: return p->noise ? p->noise : NAN;
    case S_ONLINE: return p->online != MET_NONE_U16 ? p->online : NAN;
    case S_RXPH: return p->rxph != MET_NONE_U16 ? p->rxph : NAN;
    default: return p->temp != MET_NONE_TEMP ? p->temp / 10.0f : NAN;
    }
}

// Smallest span a graph covers, so noise doesn't fill the box
static const float min_span[] = {[S_MV] = 0.1f, [S_BATT] = 5,   [S_CHUTIL] = 2,
                                 [S_TEMP] = 2,   [S_AIRTX] = 0.5f, [S_NOISE] = 4,
                                 [S_ONLINE] = 2, [S_RXPH] = 10};

typedef struct {
    int n;                // points with a value
    float lo, hi;         // of the values
    float min, max;       // the range drawn (lo..hi widened to min_span)
    uint32_t t0, t1;      // first and last point's time
} range_t;

static range_t range_of(const mesh_metrics_t *e, series_t s) {
    range_t r = {0};
    for (int i = 0; i < e->nhist; i++) {
        const met_point_t *p = mesh_metrics_point(e, i);
        float v = value(p, s);
        if (isnan(v))
            continue;
        if (!r.n++) {
            r.lo = r.hi = v;
            r.t0 = p->time;
        }
        r.lo = v < r.lo ? v : r.lo;
        r.hi = v > r.hi ? v : r.hi;
        r.t1 = p->time;
    }
    r.min = r.lo, r.max = r.hi;
    if (r.max - r.min < min_span[s]) {
        float mid = (r.max + r.min) / 2;
        r.min = mid - min_span[s] / 2;
        r.max = mid + min_span[s] / 2;
    }
    return r;
}

// The series as a line in the box, time along x. Points join up; one alone
// is a dot.
static void plot(int x, int y, int w, int h, const mesh_metrics_t *e, series_t s, uint8_t c) {
    range_t r = range_of(e, s);
    if (!r.n)
        return;
    uint32_t span = r.t1 - r.t0;
    int px = -1, py = -1;
    for (int i = 0; i < e->nhist; i++) {
        const met_point_t *p = mesh_metrics_point(e, i);
        float v = value(p, s);
        if (isnan(v))
            continue;
        int xx = x + (span ? (int)((uint64_t)(p->time - r.t0) * (uint32_t)(w - 1) / span) : w - 1);
        int yy = y + h - 1 - (int)lroundf((v - r.min) / (r.max - r.min) * (float)(h - 1));
        if (px >= 0)
            fb_line(px, py, xx, yy, c);
        else
            fb_pixel(xx, yy, c);
        px = xx, py = yy;
    }
}

// --- formatting ----------------------------------------------------------------

static const char *fmt_uptime(uint32_t s, char *buf, size_t len) {
    if (s >= 86400)
        snprintf(buf, len, "%ud %uh", (unsigned)(s / 86400), (unsigned)(s % 86400 / 3600));
    else if (s >= 3600)
        snprintf(buf, len, "%uh %um", (unsigned)(s / 3600), (unsigned)(s % 3600 / 60));
    else
        snprintf(buf, len, "%um", (unsigned)(s / 60));
    return buf;
}

static const char *fmt_batt(const meshtastic_DeviceMetrics *d, char *buf, size_t len) {
    if (!d->has_battery_level)
        return "";
    if (d->battery_level > 100)
        return "pwr";
    snprintf(buf, len, "%u%%", (unsigned)d->battery_level);
    return buf;
}

// --- list view -----------------------------------------------------------------

// A clock face, 5 px across, in the cell at x
static void clock_mark(int x, int y, uint8_t c) {
    x += 1, y += UI_LH / 2 - 3;
    fb_hline(x + 1, y, 3, c);
    fb_hline(x + 1, y + 4, 3, c);
    fb_vline(x, y + 1, 3, c);
    fb_vline(x + 4, y + 1, 3, c);
    fb_pixel(x + 2, y + 1, c);
    fb_pixel(x + 2, y + 2, c);
    fb_pixel(x + 3, y + 2, c);
}

static void spark(int col, int y, const mesh_metrics_t *e, uint8_t c) {
    range_t r = range_of(e, S_MV);
    if (r.n < 2)
        return;
    plot(col * UI_CW, y + 2, W_SPARK * UI_CW - 2, UI_LH - 4, e, S_MV, c);
}

static void row(int y, const mesh_node_t *n, bool selected) {
    uint8_t fg = selected ? FB_BLACK : FB_WHITE;
    uint8_t dim = selected ? FB_BLACK : FB_GREY;
    const mesh_metrics_t *e = mesh_metrics_find(n->num);
    char buf[16];

    if (selected)
        fb_fill(0, y, FB_W, UI_LH, FB_WHITE);
    if (n->is_favorite)
        ui_cells(C_FAV, y, 1, "*", fg, FB_CLEAR);
    ui_cells(C_SHORT, y, 4, ui_node_short(n->num), fg, FB_CLEAR);
    if (metsched_find(n->num))
        clock_mark(C_SCHED * UI_CW, y, fg);

    if (!e || !e->have[MET_DEVICE]) {
        // The Nodes tab's battery, from its NodeInfo, is all there is
        if (n->battery == MESH_BATTERY_POWERED)
            ui_cells_right(C_BATT, y, 4, "pwr", dim, FB_CLEAR);
        else if (n->battery != MESH_BATTERY_UNKNOWN) {
            snprintf(buf, sizeof buf, "%u%%", n->battery);
            ui_cells_right(C_BATT, y, 4, buf, dim, FB_CLEAR);
        }
    }
    if (!e) {
        ui_cells(C_SPARK, y, 20, "-", dim, FB_CLEAR);
        return;
    }
    const meshtastic_DeviceMetrics *d = &e->device;
    if (e->have[MET_DEVICE]) {
        bool low = d->has_battery_level && d->battery_level < 20;
        ui_cells_right(C_BATT, y, 4, fmt_batt(d, buf, sizeof buf), low && !selected ? FB_WHITE : fg,
                       FB_CLEAR);
        if (d->has_voltage && d->voltage > 0) {
            snprintf(buf, sizeof buf, "%.2f", (double)d->voltage);
            ui_cells_right(C_VOLT, y, 5, buf, fg, FB_CLEAR);
        }
        if (d->has_channel_utilization) {
            snprintf(buf, sizeof buf, "%.1f", (double)d->channel_utilization);
            ui_cells_right(C_CHU, y, 5, buf, fg, FB_CLEAR);
        }
        if (d->has_air_util_tx) {
            snprintf(buf, sizeof buf, "%.1f", (double)d->air_util_tx);
            ui_cells_right(C_AIR, y, 5, buf, fg, FB_CLEAR);
        }
    }
    spark(C_SPARK, y, e, fg);
    if (e->have[MET_ENV] && e->env.has_temperature) {
        snprintf(buf, sizeof buf, "%.1f°", (double)e->env.temperature);
        ui_cells_right(C_TEMP, y, 6, buf, fg, FB_CLEAR);
    }
    ui_cells_right(C_AGE, y, 5, ui_ago(newest(e), buf), dim, FB_CLEAR);
}

// --- requests ------------------------------------------------------------------

static bool status_dismissed = true;

static const char *req_text(uint32_t node) {
    static char buf[80];
    met_req_state_t st = mesh_metrics_req_state();
    if (st == MET_REQ_IDLE || status_dismissed || mesh_metrics_req_node() != node)
        return NULL;
    const char *who = ui_node_short(node);
    const char *what = mesh_metrics_kind_name(mesh_metrics_req_kind());
    if (st == MET_REQ_WAITING) {
        snprintf(buf, sizeof buf, "Asking %s for %s... %us", who, what,
                 (unsigned)(mesh_metrics_req_age_ms() / 1000));
        return buf;
    }
    if (st == MET_REQ_DONE) {
        snprintf(buf, sizeof buf, "%s answered with %s.", who, what);
        return buf;
    }
    switch (mesh_metrics_req_error()) {
    case MET_ERR_NO_ANSWER:
        snprintf(buf, sizeof buf, "No answer from %s for %s.", who, what);
        break;
    case MET_ERR_NO_LINK:
        snprintf(buf, sizeof buf, "Not connected to our node.");
        break;
    case meshtastic_Routing_Error_NO_RESPONSE:
        snprintf(buf, sizeof buf, "%s has no %s to send (no sensor?)", who, what);
        break;
    case meshtastic_Routing_Error_NO_ROUTE:
    case meshtastic_Routing_Error_MAX_RETRANSMIT:
    case meshtastic_Routing_Error_TIMEOUT:
        snprintf(buf, sizeof buf, "Couldn't reach %s.", who);
        break;
    case meshtastic_Routing_Error_DUTY_CYCLE_LIMIT:
        snprintf(buf, sizeof buf, "Duty cycle limit: try again later.");
        break;
    default:
        snprintf(buf, sizeof buf, "%s refused: routing error %u.", who,
                 (unsigned)mesh_metrics_req_error());
        break;
    }
    return buf;
}

static bool ask(uint32_t node, met_kind_t k) {
    status_dismissed = false;
    mesh_metrics_request(node, k);
    return true;
}

// --- a node's page -------------------------------------------------------------

static bool detail;
static int scroll;     // pixels
static int content_h;  // of the last page drawn

#define D_TOP (UI_BODY_Y + UI_LH + 2) // below the node's title bar
#define D_H (HINT_Y - 2 - D_TOP)

// Two label/value pairs to a line
#define C_L1 1
#define C_V1 12
#define C_L2 27
#define C_V2 38
#define W_LABEL 10
#define W_VALUE 14

typedef struct {
    int y;
    int half; // 0: next pair goes left, 1: right
} page_t;

static void pair(page_t *pg, const char *label, const char *val) {
    int cl = pg->half ? C_L2 : C_L1, cv = pg->half ? C_V2 : C_V1;
    ui_cells(cl, pg->y, W_LABEL, label, FB_GREY, FB_CLEAR);
    ui_cells(cv, pg->y, W_VALUE, val, FB_WHITE, FB_CLEAR);
    if (pg->half)
        pg->y += UI_LH;
    pg->half ^= 1;
}

static void end_pairs(page_t *pg) {
    if (pg->half)
        pg->y += UI_LH;
    pg->half = 0;
}

// "Name ---------- right"
static void rule(page_t *pg, const char *name, const char *right) {
    end_pairs(pg);
    pg->y += pg->y > D_TOP - scroll ? UI_LH / 2 : 0;
    int y = pg->y;
    // A cell short of the edge, clear of the scroll bar
    int nl = utf8_cells(name), nr = utf8_cells(right);
    ui_cells(0, y, nl, name, FB_WHITE, FB_CLEAR);
    ui_cells_right(UI_COLS - 1 - nr, y, nr, right, FB_GREY, FB_CLEAR);
    fb_hline((nl + 1) * UI_CW, y + UI_LH / 2, (UI_COLS - nr - nl - 3) * UI_CW, FB_DIM);
    pg->y += UI_LH;
}

// "Device ---- 12m ago ---- D: ask"
static void heading(page_t *pg, const char *name, const mesh_metrics_t *e, met_kind_t k,
                    char key) {
    char right[24], age[8];
    snprintf(right, sizeof right, "%s  %c: ask",
             e && e->have[k] ? ui_ago(e->time[k], age) : "never", key);
    rule(pg, name, right);
}

// A titled graph of one series, if there are two points or more and they
// aren't all the same
#define GRAPH_H 30

static void graph(page_t *pg, const mesh_metrics_t *e, series_t s, const char *name,
                  const char *fmt, const char *unit) {
    range_t r = range_of(e, s);
    if (r.n < 2 || r.lo == r.hi)
        return;
    end_pairs(pg);
    char title[64], lo[12], hi[12], span[8];
    snprintf(lo, sizeof lo, fmt, (double)r.lo);
    snprintf(hi, sizeof hi, fmt, (double)r.hi);
    // How far back it goes
    uint32_t back = r.t1 - r.t0;
    if (back >= 86400)
        snprintf(span, sizeof span, "%ud", (unsigned)((back + 43200) / 86400));
    else if (back >= 3600)
        snprintf(span, sizeof span, "%uh", (unsigned)((back + 1800) / 3600));
    else
        snprintf(span, sizeof span, "%um", (unsigned)((back + 30) / 60));
    snprintf(title, sizeof title, "%s %s to %s%s, last %s", name, lo, hi, unit, span);
    ui_cells(C_L1, pg->y, UI_COLS - 2, title, FB_GREY, FB_CLEAR);
    pg->y += UI_LH + 1;
    int x = C_L1 * UI_CW, w = FB_W - 2 * x;
    fb_rect(x, pg->y, w, GRAPH_H, FB_DIM);
    // Quarter lines, for reading levels off
    for (int i = 1; i < 4; i++)
        for (int xx = x + 2; xx < x + w - 2; xx += 4)
            fb_pixel(xx, pg->y + i * GRAPH_H / 4, FB_DIM);
    plot(x + 2, pg->y + 2, w - 4, GRAPH_H - 4, e, s, FB_WHITE);
    pg->y += GRAPH_H + 3;
}

// --- the schedule ----------------------------------------------------------------

static bool editing;
static uint8_t ed_kinds;
static uint16_t ed_every;
static const char *sched_msg; // instead of the hint, until a key

static const char *const kind_words[MET_KINDS] = {
    [MET_DEVICE] = "device", [MET_ENV] = "env", [MET_POWER] = "power", [MET_LOCAL] = "local"};

static const char *fmt_every(uint16_t min, char *buf, size_t len) {
    if (min % 60)
        snprintf(buf, len, "%um", min);
    else
        snprintf(buf, len, "%uh", min / 60);
    return buf;
}

static const char *fmt_kinds(uint8_t kinds, char *buf, size_t len) {
    buf[0] = 0;
    for (int k = 0; k < MET_KINDS; k++)
        if (kinds & MET_BIT(k))
            snprintf(buf + strlen(buf), len - strlen(buf), "%s%s", buf[0] ? ", " : "",
                     kind_words[k]);
    return buf;
}

// A label and a value as wide as the page
static void line(page_t *pg, const char *label, const char *val) {
    end_pairs(pg);
    ui_cells(C_L1, pg->y, W_LABEL, label, FB_GREY, FB_CLEAR);
    ui_cells(C_V1, pg->y, UI_COLS - C_V1 - 2, val, FB_WHITE, FB_CLEAR);
    pg->y += UI_LH;
}

static void sched_edit(page_t *pg) {
    end_pairs(pg);
    ui_cells(C_L1, pg->y, W_LABEL, "Ask for", FB_GREY, FB_CLEAR);
    int col = C_V1;
    for (int k = 0; k < MET_KINDS; k++) {
        bool on = ed_kinds & MET_BIT(k);
        int w = (int)strlen(kind_words[k]) + 2;
        char word[12];
        snprintf(word, sizeof word, " %s ", kind_words[k]);
        word[1] = (char)(word[1] - 'a' + 'A');
        ui_cells(col, pg->y, w, word, on ? FB_BLACK : FB_GREY, on ? FB_WHITE : FB_CLEAR);
        col += w + 1;
    }
    pg->y += UI_LH;
    char buf[16], e[8];
    ui_cells(C_L1, pg->y, W_LABEL, "Every", FB_GREY, FB_CLEAR);
    snprintf(buf, sizeof buf, " %s ", fmt_every(ed_every, e, sizeof e));
    ui_cells(C_V1, pg->y, utf8_cells(buf), buf, FB_BLACK, FB_WHITE);
    pg->y += UI_LH;
}

static void sched_section(page_t *pg, uint32_t node) {
    const met_sched_t *s = metsched_find(node);
    char right[32], buf[64], a[8], b[40];
    if (editing) {
        rule(pg, "Schedule", "Enter: save");
        sched_edit(pg);
        return;
    }
    if (!s) {
        rule(pg, "Schedule", "off  S: set");
        return;
    }
    snprintf(right, sizeof right, "every %s  S: change", fmt_every(s->every_min, a, sizeof a));
    rule(pg, "Schedule", right);
    line(pg, "Asks for", fmt_kinds(s->kinds, b, sizeof b));
    met_kind_t k;
    int try_no;
    if (metsched_running(node, &k, &try_no)) {
        if (k < MET_KINDS)
            snprintf(buf, sizeof buf, "asking for %s%s", kind_words[k],
                     try_no > 1 ? ", once more" : "");
        else
            snprintf(buf, sizeof buf, "running");
    } else if (!s->placed || mesh_link_state() != MESH_LINK_READY) {
        snprintf(buf, sizeof buf, "once connected");
    } else {
        uint32_t m = (metsched_due_in_ms(s) + 59999) / 60000;
        if (m >= 60)
            snprintf(buf, sizeof buf, "in %luh %02lum", (unsigned long)(m / 60),
                     (unsigned long)(m % 60));
        else
            snprintf(buf, sizeof buf, "in %lum", (unsigned long)m);
    }
    line(pg, "Next", buf);
    if (s->last_epoch) {
        uint8_t missed = s->last_failed;
        if (!s->last_ok)
            snprintf(buf, sizeof buf, "%s ago: nothing came back", ui_ago(s->last_epoch, a));
        else if (missed)
            snprintf(buf, sizeof buf, "%s ago: no %s", ui_ago(s->last_epoch, a),
                     fmt_kinds(missed, b, sizeof b));
        else
            snprintf(buf, sizeof buf, "%s ago: got it all", ui_ago(s->last_epoch, a));
        line(pg, "Last run", buf);
    }
}

static void sched_key_start(uint32_t node) {
    const met_sched_t *s = metsched_find(node);
    editing = true;
    ed_kinds = s ? s->kinds : (uint8_t)(MET_BIT(MET_DEVICE) | MET_BIT(MET_LOCAL));
    ed_every = s ? s->every_min : 60;
    scroll = INT_MAX / 2; // to the end, where it is; drawing pulls it back
}

static bool sched_key(uint16_t k) {
    switch (k) {
    case 'd': ed_kinds ^= MET_BIT(MET_DEVICE); break;
    case 'e': ed_kinds ^= MET_BIT(MET_ENV); break;
    case 'p': ed_kinds ^= MET_BIT(MET_POWER); break;
    case 'l': ed_kinds ^= MET_BIT(MET_LOCAL); break;
    case KBD_LEFT:
    case '-': ed_every = metsched_step(ed_every, -1); break;
    case KBD_RIGHT:
    case '+': ed_every = metsched_step(ed_every, 1); break;
    case KBD_ENTER:
        editing = false;
        if (!metsched_set(sel_num, ed_kinds, ed_every))
            sched_msg = "All " TOSTR(METSCHED_MAX) " schedules are in use.";
        break;
    case KBD_ESC:
    case KBD_BACKSPACE: editing = false; break;
    default: break;
    }
    return true;
}

static void device_section(page_t *pg, const mesh_metrics_t *e) {
    heading(pg, "Device", e, MET_DEVICE, 'D');
    if (!e || !e->have[MET_DEVICE])
        return;
    const meshtastic_DeviceMetrics *d = &e->device;
    char buf[24];
    if (d->has_battery_level) {
        char b[8];
        if (d->has_voltage && d->voltage > 0)
            snprintf(buf, sizeof buf, "%s  %.2f V", fmt_batt(d, b, sizeof b), (double)d->voltage);
        else
            snprintf(buf, sizeof buf, "%s", fmt_batt(d, b, sizeof b));
        pair(pg, "Battery", buf);
    } else if (d->has_voltage) {
        snprintf(buf, sizeof buf, "%.2f V", (double)d->voltage);
        pair(pg, "Battery", buf);
    }
    if (d->has_uptime_seconds)
        pair(pg, "Uptime", fmt_uptime(d->uptime_seconds, buf, sizeof buf));
    if (d->has_channel_utilization) {
        snprintf(buf, sizeof buf, "%.1f%%", (double)d->channel_utilization);
        pair(pg, "Ch util", buf);
    }
    if (d->has_air_util_tx) {
        snprintf(buf, sizeof buf, "%.2f%%", (double)d->air_util_tx);
        pair(pg, "Air TX", buf);
    }
    if (range_of(e, S_MV).n >= 2)
        graph(pg, e, S_MV, "Battery", "%.2f", " V");
    else
        graph(pg, e, S_BATT, "Battery", "%.0f", "%");
    graph(pg, e, S_CHUTIL, "Ch util", "%.1f", "%");
    graph(pg, e, S_AIRTX, "Air TX", "%.2f", "%");
}

// Environment metrics: every float or integer the node said it has
typedef struct {
    const char *label;
    uint16_t has, val;
    uint8_t size; // of an unsigned field (telemetry.options narrows some); 0 float
    const char *fmt;
} env_field_t;

#define EM meshtastic_EnvironmentMetrics
#define EF(f, lbl, fmt) {lbl, offsetof(EM, has_##f), offsetof(EM, f), 0, fmt}
#define EU(f, lbl, fmt) {lbl, offsetof(EM, has_##f), offsetof(EM, f), sizeof(((EM *)0)->f), fmt}

static void env_section(page_t *pg, const mesh_metrics_t *e) {
    heading(pg, "Environment", e, MET_ENV, 'E');
    if (!e || !e->have[MET_ENV])
        return;
    const meshtastic_EnvironmentMetrics *v = &e->env;
    char buf[24];
    static const env_field_t fields[] = {
        EF(temperature, "Temp", "%.1f°C"),
        EF(relative_humidity, "Humidity", "%.0f%%"),
        EF(barometric_pressure, "Pressure", "%.1f hPa"),
        EF(gas_resistance, "Gas", "%.2f MOhm"),
        EU(iaq, "IAQ", "%u"),
        EF(lux, "Light", "%.0f lx"),
        EF(white_lux, "White", "%.0f lx"),
        EF(uv_lux, "UV", "%.1f lx"),
        EF(ir_lux, "IR", "%.0f lx"),
        EF(wind_speed, "Wind", "%.1f m/s"),
        EU(wind_direction, "Wind from", "%u°"),
        EF(wind_gust, "Gusts", "%.1f m/s"),
        EF(wind_lull, "Lulls", "%.1f m/s"),
        EF(rainfall_1h, "Rain 1h", "%.1f mm"),
        EF(rainfall_24h, "Rain 24h", "%.1f mm"),
        EU(soil_moisture, "Soil", "%u%%"),
        EF(soil_temperature, "Soil temp", "%.1f°C"),
        EF(voltage, "Voltage", "%.2f V"),
        EF(current, "Current", "%.1f mA"),
        EF(distance, "Distance", "%.0f mm"),
        EF(weight, "Weight", "%.2f kg"),
        EF(radiation, "Radiation", "%.2f µR/h"),
        EU(lightning_strike_count_1h, "Strikes 1h", "%u"),
        EF(lightning_distance_km, "Lightning", "%.0f km"),
    };
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        const env_field_t *f = &fields[i];
        const char *base = (const char *)v;
        if (!*(const bool *)(base + f->has))
            continue;
        if (f->size) {
            const void *p = base + f->val;
            unsigned u = f->size == 1   ? *(const uint8_t *)p
                         : f->size == 2 ? *(const uint16_t *)p
                                        : (unsigned)*(const uint32_t *)p;
            snprintf(buf, sizeof buf, f->fmt, u);
        } else {
            snprintf(buf, sizeof buf, f->fmt, (double)*(const float *)(base + f->val));
        }
        pair(pg, f->label, buf);
    }
    graph(pg, e, S_TEMP, "Temp", "%.1f", "°C");
}

static void power_section(page_t *pg, const mesh_metrics_t *e) {
    heading(pg, "Power", e, MET_POWER, 'P');
    if (!e || !e->have[MET_POWER])
        return;
    const meshtastic_PowerMetrics *w = &e->power;
    const struct {
        bool hv, hc;
        float v, c;
    } ch[] = {
        {w->has_ch1_voltage, w->has_ch1_current, w->ch1_voltage, w->ch1_current},
        {w->has_ch2_voltage, w->has_ch2_current, w->ch2_voltage, w->ch2_current},
        {w->has_ch3_voltage, w->has_ch3_current, w->ch3_voltage, w->ch3_current},
    };
    char label[8], buf[24], v[12], c[12];
    for (int i = 0; i < 3; i++) {
        if (!ch[i].hv && !ch[i].hc)
            continue;
        snprintf(label, sizeof label, "Ch %d", i + 1);
        snprintf(v, sizeof v, ch[i].hv ? "%.2f V" : "", (double)ch[i].v);
        snprintf(c, sizeof c, ch[i].hc ? "%.1f mA" : "", (double)ch[i].c);
        snprintf(buf, sizeof buf, "%-8s %s", v, c);
        end_pairs(pg);
        ui_cells(C_L1, pg->y, W_LABEL, label, FB_GREY, FB_CLEAR);
        ui_cells(C_V1, pg->y, 30, buf, FB_WHITE, FB_CLEAR);
        pg->y += UI_LH;
    }
}

static void local_section(page_t *pg, const mesh_metrics_t *e) {
    heading(pg, "Local stats", e, MET_LOCAL, 'L');
    if (!e || !e->have[MET_LOCAL])
        return;
    const meshtastic_LocalStats *l = &e->local;
    char buf[24];
#define U(label, v)                                                            \
    do {                                                                       \
        snprintf(buf, sizeof buf, "%lu", (unsigned long)(v));                  \
        pair(pg, label, buf);                                                  \
    } while (0)
    U("Sent", l->num_packets_tx);
    U("Received", l->num_packets_rx);
    U("Bad", l->num_packets_rx_bad);
    U("Dupes", l->num_rx_dupe);
    U("Relayed", l->num_tx_relay);
    U("Relays cut", l->num_tx_relay_canceled);
    U("TX dropped", l->num_tx_dropped);
    snprintf(buf, sizeof buf, "%u of %u", (unsigned)l->num_online_nodes,
             (unsigned)l->num_total_nodes);
    pair(pg, "Online", buf);
    snprintf(buf, sizeof buf, "%.1f%%", (double)l->channel_utilization);
    pair(pg, "Ch util", buf);
    snprintf(buf, sizeof buf, "%.2f%%", (double)l->air_util_tx);
    pair(pg, "Air TX", buf);
    if (l->noise_floor) {
        snprintf(buf, sizeof buf, "%ld dBm", (long)l->noise_floor);
        pair(pg, "Noise", buf);
    }
    pair(pg, "Uptime", fmt_uptime(l->uptime_seconds, buf, sizeof buf));
    if (l->heap_total_bytes) {
        snprintf(buf, sizeof buf, "%lu of %lu kB", (unsigned long)(l->heap_free_bytes / 1024),
                 (unsigned long)(l->heap_total_bytes / 1024));
        pair(pg, "Heap free", buf);
    }
#undef U
    end_pairs(pg);
    graph(pg, e, S_RXPH, "Heard", "%.0f", "/h");
    graph(pg, e, S_ONLINE, "Online", "%.0f", " nodes");
    graph(pg, e, S_NOISE, "Noise", "%.0f", " dBm");
}

static void fix_scroll(void) {
    int max = content_h - D_H;
    if (scroll > max)
        scroll = max;
    if (scroll < 0)
        scroll = 0;
}

static void draw_detail(void) {
    const mesh_node_t *n = mesh_db_find(sel_num);
    if (!n) {
        detail = false;
        return;
    }
    bool me = n->num == mesh_db.my_node_num;
    const mesh_metrics_t *e = mesh_metrics_find(n->num);

    // Title bar
    int y = UI_BODY_Y;
    fb_fill(0, y, FB_W, UI_LH + 1, FB_WHITE);
    char title[64], id[12];
    snprintf(title, sizeof title, "%s  %s%s", ui_node_short(n->num), ui_node_long(n->num),
             me ? "  (ours)" : "");
    snprintf(id, sizeof id, "!%08x", (unsigned)n->num);
    ui_cells(0, y + 1, UI_COLS - 11, title, FB_BLACK, FB_CLEAR);
    ui_cells_right(UI_COLS - 10, y + 1, 10, id, FB_BLACK, FB_CLEAR);

    // The page, scrolled. Its height is only known once drawn, and new
    // metrics change it, so a scroll past the end is pulled back and drawn
    // again.
    fb_clip(0, D_TOP, FB_W, D_H);
    for (int pass = 0; pass < 2; pass++) {
        page_t pg = {.y = D_TOP - scroll};
        device_section(&pg, e);
        env_section(&pg, e);
        power_section(&pg, e);
        local_section(&pg, e);
        sched_section(&pg, n->num);
        end_pairs(&pg);
        content_h = pg.y + scroll - D_TOP;
        int was = scroll;
        fix_scroll();
        if (scroll == was)
            break;
        fb_fill(0, D_TOP, FB_W, D_H, FB_BLACK);
    }
    fb_clip(0, UI_BODY_Y, FB_W, UI_BODY_H);

    if (content_h > D_H) {
        int bh = D_H * D_H / content_h, by = D_TOP + D_H * scroll / content_h;
        fb_vline(FB_W - 2, D_TOP, D_H, FB_DIM);
        fb_fill(FB_W - 3, by, 3, bh < 4 ? 4 : bh, FB_WHITE);
    }

    fb_hline(0, HINT_Y - 1, FB_W, FB_DIM);
    const char *st = editing ? NULL : sched_msg ? sched_msg : req_text(n->num);
    if (st)
        ui_cells(0, HINT_Y, UI_COLS, st, FB_WHITE, FB_CLEAR);
    else
        ui_cells(0, HINT_Y, UI_COLS,
                 editing ? "D E P L: pick  Left/Right: how often  Enter: save"
                         : "D E P L: ask  S: schedule  Esc: list",
                 FB_DIM, FB_CLEAR);
}

static bool detail_key(const kbd_event_t *ev) {
    int n = mesh_db.node_count;
    uint16_t k = ev->key;
    if (k >= 'A' && k <= 'Z')
        k = (uint16_t)(k - 'A' + 'a');
    sched_msg = NULL;
    if (editing)
        return sched_key(k);
    switch (k) {
    case 's': sched_key_start(sel_num); return true;
    case 'd': return ask(sel_num, MET_DEVICE);
    case 'e': return ask(sel_num, MET_ENV);
    case 'p': return ask(sel_num, MET_POWER);
    case 'l': return ask(sel_num, MET_LOCAL);
    default: break;
    }
    // An answer's line stays while the page is read, until we leave it
    bool done = mesh_metrics_req_state() != MET_REQ_WAITING;
    switch (k) {
    case KBD_ESC:
    case KBD_BACKSPACE:
        status_dismissed |= done;
        detail = false;
        return true;
    case KBD_UP: scroll -= UI_LH; break;
    case KBD_DOWN: scroll += UI_LH; break;
    case KBD_PGUP: scroll -= D_H - UI_LH; break;
    case KBD_PGDN: scroll += D_H - UI_LH; break;
    case KBD_HOME: scroll = 0; break;
    case KBD_END: scroll = INT_MAX / 2; break; // drawing pulls it back
    case KBD_LEFT:
    case KBD_RIGHT:
        // The list's previous or next node
        sort();
        if (!n)
            return false;
        status_dismissed |= done;
        list.sel += k == KBD_LEFT ? -1 : 1;
        ui_list_fix(&list, n, ROWS);
        sel_num = mesh_db.nodes[order[list.sel]].num;
        scroll = 0;
        return true;
    default:
        return false;
    }
    if (scroll < 0) // past the end is pulled back when drawn
        scroll = 0;
    return true;
}

// --- the tab -------------------------------------------------------------------

void ui_metrics_draw(void) {
    int n = sort();
    if (detail && n) {
        draw_detail();
        if (detail)
            return;
    }
    detail = false;

    int y = UI_BODY_Y + 1;
    ui_cells(C_SHORT, y, 4, "Name", FB_DIM, FB_CLEAR);
    ui_cells_right(C_BATT, y, 4, "Bat", FB_DIM, FB_CLEAR);
    ui_cells_right(C_VOLT, y, 5, "Volts", FB_DIM, FB_CLEAR);
    ui_cells(C_SPARK, y, W_SPARK, "trend", FB_DIM, FB_CLEAR);
    ui_cells_right(C_CHU, y, 5, "ChU%", FB_DIM, FB_CLEAR);
    ui_cells_right(C_AIR, y, 5, "Air%", FB_DIM, FB_CLEAR);
    ui_cells_right(C_TEMP, y, 6, "Temp", FB_DIM, FB_CLEAR);
    ui_cells_right(C_AGE, y, 5, "Age", FB_DIM, FB_CLEAR);
    y += UI_LH + 1;
    fb_hline(0, y - 1, FB_W, FB_DIM);

    if (!n) {
        ui_cells(1, y + UI_LH, UI_COLS - 2, "No nodes yet.", FB_GREY, FB_CLEAR);
        return;
    }
    for (int i = list.top; i < n && i < list.top + ROWS; i++, y += UI_LH)
        row(y, &mesh_db.nodes[order[i]], i == list.sel);

    if (n > ROWS) {
        int top = UI_BODY_Y + UI_LH + 2, h = ROWS * UI_LH;
        int bh = h * ROWS / n, by = top + h * list.top / n;
        fb_vline(FB_W - 2, top, h, FB_DIM);
        fb_fill(FB_W - 3, by, 3, bh < 4 ? 4 : bh, FB_WHITE);
    }

    fb_hline(0, HINT_Y - 1, FB_W, FB_DIM);
    const char *st = req_text(mesh_metrics_req_node());
    ui_cells(0, HINT_Y, UI_COLS, st ? st : "Enter: the node's metrics, and ask it for more",
             st ? FB_WHITE : FB_DIM, FB_CLEAR);
}

bool ui_metrics_key(const kbd_event_t *ev) {
    int n = sort();
    if (!n)
        return false;
    if (detail)
        return detail_key(ev);
    if (ui_list_key(&list, n, ROWS, ev->key)) {
        sel_num = mesh_db.nodes[order[list.sel]].num;
        return true;
    }
    if (ev->key == KBD_ENTER) {
        detail = true;
        editing = false;
        sched_msg = NULL;
        scroll = 0;
        status_dismissed |= mesh_metrics_req_state() != MET_REQ_WAITING;
        return true;
    }
    return false;
}
