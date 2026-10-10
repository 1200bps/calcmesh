// Position tab: every node with a known position as a point on a plain
// black plane, north up, labelled with its short name; no map underneath.
// Our own node is a hollow square, the selected one is bracketed, and points
// fade with how long ago the node was heard. Two lines at the bottom show the
// selected node's distance, bearing and last-heard (or the keys).
//
// Keys: arrows pan, + and - zoom, 0 or Home fits everyone back in, n/p (or
// PgDn/PgUp) pick the next/previous node outward from us, c centres on it,
// Esc drops the pick, Enter opens a DM with it.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "ui_internal.h"

#define MAP_X 0
#define MAP_Y UI_BODY_Y
#define MAP_W FB_W
#define MAP_H (UI_BODY_H - 2 * UI_LH - 2)
#define INFO_Y (MAP_Y + MAP_H + 2)
#define MARGIN 14 // keep fitted points this far inside the map

#define M_PER_DEG 111194.93 // on a sphere of 6371 km

// Metres per pixel, in 1-2-5 steps from half a metre to ten km
static const float zooms[] = {0.5f, 1,    2,    5,    10,    20,    50,   100,
                              200,  500,  1000, 2000, 5000, 10000};
#define NZOOMS (int)(sizeof zooms / sizeof zooms[0])

static struct {
    bool manual;          // panned or zoomed since the last fit
    int zoom;             // index into zooms
    float cx, cy;         // view centre, metres east/north of the origin
    int32_t lat0, lon0;   // origin: our node, or the first placed node
    float kx;             // metres per 1e-7 degree of longitude at lat0
    uint32_t sel;         // selected node, 0 for none
} v;

static uint16_t placed[MESH_MAX_NODES]; // indices into mesh_db.nodes
static int nplaced;

// --- geometry ----------------------------------------------------------------

static const mesh_node_t *me(void) {
    const mesh_node_t *n = mesh_db_find(mesh_db.my_node_num);
    return n && n->has_position ? n : NULL;
}

// Flat-earth offsets from the origin; good to well under a pixel at any
// zoom for a mesh's worth of distance. Deltas are taken in integers first so
// float rounding never sees the whole coordinate.
static void to_metres(const mesh_node_t *n, float *e, float *nth) {
    *e = (float)((int64_t)n->longitude_i - v.lon0) * v.kx;
    *nth = (float)((int64_t)n->latitude_i - v.lat0) * (float)(M_PER_DEG / 1e7);
}

static void to_screen(float e, float nth, int *x, int *y) {
    float mpp = zooms[v.zoom];
    *x = MAP_X + MAP_W / 2 + (int)lroundf((e - v.cx) / mpp);
    *y = MAP_Y + MAP_H / 2 - (int)lroundf((nth - v.cy) / mpp);
}

static bool on_map(int x, int y, int pad) {
    return x >= MAP_X + pad && x < MAP_X + MAP_W - pad && y >= MAP_Y + pad &&
           y < MAP_Y + MAP_H - pad;
}

// Great-circle distance (metres) and initial bearing (degrees) from a to b
static double distance(const mesh_node_t *a, const mesh_node_t *b, double *bearing) {
    double la1 = a->latitude_i * 1e-7 * M_PI / 180, la2 = b->latitude_i * 1e-7 * M_PI / 180;
    double dla = la2 - la1;
    double dlo = ((int64_t)b->longitude_i - a->longitude_i) * 1e-7 * M_PI / 180;
    double h = sin(dla / 2) * sin(dla / 2) + cos(la1) * cos(la2) * sin(dlo / 2) * sin(dlo / 2);
    double y = sin(dlo) * cos(la2);
    double x = cos(la1) * sin(la2) - sin(la1) * cos(la2) * cos(dlo);
    *bearing = fmod(atan2(y, x) * 180 / M_PI + 360, 360);
    return 2 * 6371000.0 * asin(sqrt(h < 1 ? h : 1));
}

static double dist_from_origin(const mesh_node_t *n) {
    float e, nth;
    to_metres(n, &e, &nth);
    return (double)e * e + (double)nth * nth;
}

static int cmp_dist(const void *a, const void *b) {
    const mesh_node_t *x = &mesh_db.nodes[*(const uint16_t *)a];
    const mesh_node_t *y = &mesh_db.nodes[*(const uint16_t *)b];
    double dx = dist_from_origin(x), dy = dist_from_origin(y);
    if (dx != dy)
        return dx < dy ? -1 : 1;
    return x->num < y->num ? -1 : x->num > y->num;
}

// Gather the placed nodes, nearest the origin first, and set the origin.
static void collect(void) {
    nplaced = 0;
    for (int i = 0; i < mesh_db.node_count; i++)
        if (mesh_db.nodes[i].has_position)
            placed[nplaced++] = (uint16_t)i;
    if (!nplaced)
        return;

    const mesh_node_t *o = me() ? me() : &mesh_db.nodes[placed[0]];
    if (o->latitude_i != v.lat0 || o->longitude_i != v.lon0 || v.kx == 0) {
        // The origin moved (we did, or our fix arrived): keep the view where
        // it was on the ground
        float de = 0, dn = 0;
        if (v.kx != 0)
            to_metres(o, &de, &dn);
        v.lat0 = o->latitude_i;
        v.lon0 = o->longitude_i;
        v.kx = (float)(M_PER_DEG / 1e7 * cos(v.lat0 * 1e-7 * M_PI / 180));
        v.cx -= de;
        v.cy -= dn;
    }
    qsort(placed, nplaced, sizeof placed[0], cmp_dist);
}

// Centre on everyone and zoom in as far as they all fit. A node not heard
// for a day, or heard only over MQTT (which can be anywhere on earth),
// would drag the view out to it, so those count only when nothing better
// is placed.
static bool fit_counts(const mesh_node_t *n, int pass) {
    if (pass >= 2 || n->num == mesh_db.my_node_num)
        return true;
    if (n->via_mqtt)
        return false;
    uint32_t now = ui_now();
    return pass == 1 || (n->last_heard && (now <= n->last_heard || now - n->last_heard <= 86400));
}

static void fit(void) {
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    int counted = 0;
    for (int pass = 0; pass < 3 && counted < 2; pass++) {
        counted = 0;
        for (int i = 0; i < nplaced; i++) {
            const mesh_node_t *n = &mesh_db.nodes[placed[i]];
            if (!fit_counts(n, pass))
                continue;
            float e, nth;
            to_metres(n, &e, &nth);
            if (!counted || e < x0) x0 = e;
            if (!counted || e > x1) x1 = e;
            if (!counted || nth < y0) y0 = nth;
            if (!counted || nth > y1) y1 = nth;
            counted++;
        }
    }
    v.cx = (x0 + x1) / 2;
    v.cy = (y0 + y1) / 2;
    v.zoom = 0;
    while (v.zoom < NZOOMS - 1 && ((x1 - x0) / zooms[v.zoom] > MAP_W - 2 * MARGIN ||
                                   (y1 - y0) / zooms[v.zoom] > MAP_H - 2 * MARGIN))
        v.zoom++;
    // A lone point, or a tight cluster, shouldn't land at half a metre a pixel
    if (v.zoom < 5)
        v.zoom = 5;
}

static int find_placed(uint32_t num) {
    for (int i = 0; i < nplaced; i++)
        if (mesh_db.nodes[placed[i]].num == num)
            return i;
    return -1;
}

static void centre_on(const mesh_node_t *n) {
    to_metres(n, &v.cx, &v.cy);
    v.manual = true;
}

// --- text --------------------------------------------------------------------

static const char *fmt_dist(double m, char *buf, size_t len) {
    if (m < 1000)
        snprintf(buf, len, "%d m", (int)lround(m));
    else if (m < 10000)
        snprintf(buf, len, "%.2f km", m / 1000);
    else if (m < 100000)
        snprintf(buf, len, "%.1f km", m / 1000);
    else
        snprintf(buf, len, "%ld km", lround(m / 1000));
    return buf;
}

static const char *compass(double deg) {
    static const char *const pts[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return pts[(int)((deg + 22.5) / 45) % 8];
}

// --- drawing -----------------------------------------------------------------

static uint8_t shade(const mesh_node_t *n) {
    if (n->num == mesh_db.my_node_num)
        return FB_WHITE;
    uint32_t now = ui_now(), age = now > n->last_heard ? now - n->last_heard : 0;
    if (!n->last_heard || age > 86400)
        return FB_DIM;
    return age > 3600 ? FB_GREY : FB_WHITE;
}

typedef struct {
    int16_t x, y, w, h;
} box_t;

static box_t taken[MESH_MAX_NODES + 4];
static int ntaken;

static bool overlaps(box_t b) {
    if (b.x < MAP_X || b.y < MAP_Y || b.x + b.w > MAP_X + MAP_W || b.y + b.h > MAP_Y + MAP_H)
        return true;
    for (int i = 0; i < ntaken; i++) {
        const box_t *t = &taken[i];
        if (b.x < t->x + t->w && t->x < b.x + b.w && b.y < t->y + t->h && t->y < b.y + b.h)
            return true;
    }
    return false;
}

static void reserve(int x, int y, int w, int h) {
    if (ntaken < (int)(sizeof taken / sizeof taken[0]))
        taken[ntaken++] = (box_t){(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h};
}

// Short name to the right of the point, or failing that to the left; left
// off where it would cover another label. The selected node's always shows.
static void label(const mesh_node_t *n, int x, int y, bool sel) {
    const char *s = ui_node_short(n->num);
    int w = utf8_cells(s) * UI_CW + 2, h = UI_LH, gap = sel ? 8 : 5; // clear of the brackets
    box_t right = {(int16_t)(x + gap), (int16_t)(y - h / 2), (int16_t)w, (int16_t)h};
    box_t left = {(int16_t)(x - gap - w), right.y, (int16_t)w, (int16_t)h};
    box_t b = right;
    if (overlaps(right)) {
        if (!overlaps(left))
            b = left;
        else if (!sel)
            return;
    }
    if (sel) {
        fb_fill(b.x, b.y, b.w, b.h, FB_WHITE);
        fb_text(b.x + 1, b.y, s, -1, UI_FONT, FB_BLACK, FB_CLEAR);
    } else {
        fb_text(b.x + 1, b.y, s, -1, UI_FONT, shade(n), FB_CLEAR);
    }
    reserve(b.x, b.y, b.w, b.h);
}

static void marker(const mesh_node_t *n, int x, int y, bool sel) {
    uint8_t c = shade(n);
    if (n->num == mesh_db.my_node_num) {
        fb_rect(x - 3, y - 3, 7, 7, FB_WHITE);
        fb_pixel(x, y, FB_WHITE);
    } else {
        fb_fill(x - 1, y - 1, 3, 3, c);
    }
    if (sel) { // corner brackets
        int r = 6;
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2) {
                fb_hline(sx < 0 ? x - r : x + r - 2, y + sy * r, 3, FB_WHITE);
                fb_vline(x + sx * r, sy < 0 ? y - r : y + r - 2, 3, FB_WHITE);
            }
    }
}

// A nice round distance 40-100 px long, as a bar with end ticks
static void scale_bar(void) {
    float mpp = zooms[v.zoom];
    static const double steps[] = {2, 2.5, 2}; // 1, 2, 5, 10, 20, 50...
    double d = 1;
    for (int k = 0; d / mpp < 40; k++)
        d *= steps[k % 3];
    int px = (int)lround(d / mpp);
    int x = MAP_X + 4, y = MAP_Y + MAP_H - 4;
    fb_hline(x, y, px + 1, FB_GREY);
    fb_vline(x, y - 3, 4, FB_GREY);
    fb_vline(x + px, y - 3, 4, FB_GREY);
    char buf[16];
    if (d < 1000)
        snprintf(buf, sizeof buf, "%g m", d);
    else
        snprintf(buf, sizeof buf, "%g km", d / 1000);
    fb_text(x + 2, y - 3 - UI_LH, buf, -1, UI_FONT, FB_GREY, FB_CLEAR);
    reserve(x, y - 3 - UI_LH, px + 2, UI_LH + 4);
}

// North arrow in the top right corner
static void north(void) {
    int x = MAP_X + MAP_W - 8, y = MAP_Y + 3;
    fb_text(x - 2, y + 7, "N", 1, UI_FONT, FB_GREY, FB_CLEAR);
    for (int i = 0; i < 5; i++)
        fb_hline(x + 1 - i / 2 - 1, y + i, 1 + 2 * (i / 2), FB_GREY);
    reserve(x - 6, y, 14, UI_LH + 8);
}

// Where an off-map node lies: a short tick on the edge facing it
static void edge_tick(int x, int y, uint8_t c) {
    int cx = x < MAP_X + 1 ? MAP_X + 1 : x > MAP_X + MAP_W - 2 ? MAP_X + MAP_W - 2 : x;
    int cy = y < MAP_Y + 1 ? MAP_Y + 1 : y > MAP_Y + MAP_H - 2 ? MAP_Y + MAP_H - 2 : y;
    if (cx != x)
        fb_vline(cx, cy - 2, 5, c);
    else
        fb_hline(cx - 2, cy, 5, c);
}

static void info(void) {
    fb_hline(0, INFO_Y - 2, FB_W, FB_DIM);
    int y = INFO_Y;
    char buf[64], a[16], b[24];
    int i = find_placed(v.sel);
    if (i < 0) {
        ui_cells(0, y, UI_COLS, "arrows pan  +/- zoom  0 fit  n/p pick", FB_DIM, FB_CLEAR);
        const mesh_node_t *m = me();
        if (m)
            snprintf(buf, sizeof buf, "Here %.5f %.5f", m->latitude_i * 1e-7,
                     m->longitude_i * 1e-7);
        else
            snprintf(buf, sizeof buf, "No position for this node");
        ui_cells(0, y + UI_LH, UI_COLS - 12, buf, FB_GREY, FB_CLEAR);
        snprintf(buf, sizeof buf, "%d/%u placed", nplaced, (unsigned)mesh_db.node_count);
        ui_cells_right(UI_COLS - 12, y + UI_LH, 12, buf, FB_GREY, FB_CLEAR);
        return;
    }

    const mesh_node_t *n = &mesh_db.nodes[placed[i]];
    bool self = n->num == mesh_db.my_node_num;
    snprintf(buf, sizeof buf, "%-4s %s", ui_node_short(n->num), ui_node_long(n->num));
    ui_cells(0, y, UI_COLS - 11, buf, FB_WHITE, FB_CLEAR);
    if (self)
        snprintf(buf, sizeof buf, "this node");
    else
        snprintf(buf, sizeof buf, "heard %s", ui_ago(n->last_heard, a));
    ui_cells_right(UI_COLS - 11, y, 11, buf, FB_GREY, FB_CLEAR);

    y += UI_LH;
    const mesh_node_t *m = me();
    if (self || !m) {
        snprintf(buf, sizeof buf, "%.5f %.5f", n->latitude_i * 1e-7, n->longitude_i * 1e-7);
    } else {
        double brg, d = distance(m, n, &brg);
        snprintf(buf, sizeof buf, "%s  %03d\xc2\xb0 %s", fmt_dist(d, a, sizeof a),
                 (int)lround(brg) % 360, compass(brg));
    }
    ui_cells(0, y, 30, buf, FB_WHITE, FB_CLEAR);
    b[0] = 0;
    if (n->altitude)
        snprintf(b, sizeof b, "alt %ldm", (long)n->altitude);
    if (!self && n->hops_away != MESH_HOPS_UNKNOWN)
        snprintf(b + strlen(b), sizeof b - strlen(b), "%shop %u", b[0] ? "  " : "",
                 n->hops_away);
    ui_cells_right(30, y, UI_COLS - 30, b, FB_GREY, FB_CLEAR);
}

void ui_position_draw(void) {
    collect();
    if (!v.manual && nplaced)
        fit();
    ntaken = 0;

    if (!nplaced) {
        ui_cells(1, MAP_Y + UI_LH, UI_COLS - 2, "No positions yet.", FB_GREY, FB_CLEAR);
        ui_cells(1, MAP_Y + 2 * UI_LH, UI_COLS - 2,
                 "Nodes appear here once they share one.", FB_DIM, FB_CLEAR);
        info();
        return;
    }

    north();
    scale_bar();

    // Points first, faded and far ones underneath; then labels, most
    // important first so they win the space: selected, us, nearest out.
    fb_clip(MAP_X, MAP_Y, MAP_W, MAP_H);
    int sel = find_placed(v.sel);
    for (int i = nplaced - 1; i >= 0; i--) {
        const mesh_node_t *n = &mesh_db.nodes[placed[i]];
        float e, nth;
        int x, y;
        to_metres(n, &e, &nth);
        to_screen(e, nth, &x, &y);
        if (i == sel)
            continue; // drawn last, on top
        if (on_map(x, y, 0))
            marker(n, x, y, false);
        else
            edge_tick(x, y, FB_DIM);
    }
    if (sel >= 0) {
        const mesh_node_t *n = &mesh_db.nodes[placed[sel]];
        float e, nth;
        int x, y;
        to_metres(n, &e, &nth);
        to_screen(e, nth, &x, &y);
        if (on_map(x, y, 0))
            marker(n, x, y, true);
        else
            edge_tick(x, y, FB_WHITE);
    }
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < nplaced; i++) {
            const mesh_node_t *n = &mesh_db.nodes[placed[i]];
            bool self = n->num == mesh_db.my_node_num;
            if ((pass == 0) != (i == sel) || (pass == 1) != (self && i != sel))
                continue;
            float e, nth;
            int x, y;
            to_metres(n, &e, &nth);
            to_screen(e, nth, &x, &y);
            if (on_map(x, y, 0)) {
                reserve(x - 3, y - 3, 7, 7); // the marker
                label(n, x, y, i == sel);
            }
        }
    fb_clip(0, UI_BODY_Y, FB_W, UI_BODY_H);
    info();
}

static void pick(int step) {
    if (!nplaced)
        return;
    int i = find_placed(v.sel);
    if (i < 0)
        i = step > 0 ? 0 : nplaced - 1;
    else
        i = (i + step + nplaced) % nplaced;
    const mesh_node_t *n = &mesh_db.nodes[placed[i]];
    v.sel = n->num;
    float e, nth;
    int x, y;
    to_metres(n, &e, &nth);
    to_screen(e, nth, &x, &y);
    if (!on_map(x, y, MARGIN))
        centre_on(n);
}

// Zoom about the selected node when there is one, so it stays put
static void zoom(int dz) {
    int z = v.zoom + dz;
    if (z < 0 || z >= NZOOMS)
        return;
    int i = find_placed(v.sel);
    if (i >= 0) {
        float e, nth, k = zooms[z] / zooms[v.zoom];
        to_metres(&mesh_db.nodes[placed[i]], &e, &nth);
        v.cx = e - (e - v.cx) * k;
        v.cy = nth - (nth - v.cy) * k;
    }
    v.zoom = z;
    v.manual = true;
}

bool ui_position_key(const kbd_event_t *ev) {
    collect();
    if (!v.manual && nplaced)
        fit();
    float step = zooms[v.zoom] * MAP_H / 4;
    int i = find_placed(v.sel);

    switch (ev->key) {
    case KBD_UP: v.cy += step; v.manual = true; return true;
    case KBD_DOWN: v.cy -= step; v.manual = true; return true;
    case KBD_LEFT: v.cx -= step; v.manual = true; return true;
    case KBD_RIGHT: v.cx += step; v.manual = true; return true;
    case '+': case '=': zoom(-1); return true;
    case '-': case '_': zoom(1); return true;
    case '0': case KBD_HOME: v.manual = false; return true;
    case 'n': case KBD_PGDN: pick(1); return true;
    case 'p': case KBD_PGUP: pick(-1); return true;
    case 'c':
        if (i >= 0)
            centre_on(&mesh_db.nodes[placed[i]]);
        else if (me())
            centre_on(me());
        return true;
    case KBD_ESC:
        v.sel = 0;
        return true;
    case KBD_ENTER:
        if (i >= 0 && v.sel != mesh_db.my_node_num) {
            ui_chats_open(MESH_CONV_DM(v.sel));
            return true;
        }
        return false;
    }
    return false;
}
