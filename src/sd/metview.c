#include "sd/metview.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "sd/metlog.h"

#define CHUNK 256        // bytes per read, and what a search probe reads
#define TASK_BYTES 2048  // read per metview_task()
#define STALE_MAX 14400  // seconds the window's end may move on before a reload
#define FIELDS 16        // the local stats file's columns

typedef struct {
    float v[MV_SERIES]; // a sum while loading, then the mean
    uint8_t n[MV_SERIES];
} bin_t;

static bin_t bins[METVIEW_BINS];

typedef struct {
    uint32_t node, span, end;
} want_t;

static want_t want, have; // asked for; loaded (or loading)
static bool online, loading, ready;
static uint32_t start, newest;

// The files with something graphed, in this order: device metrics first, so
// local stats' channel use only fills in where there's none from them.
static const met_kind_t files[] = {MET_DEVICE, MET_ENV, MET_LOCAL};
static const char *const file_names[] = {"device", "env", "local"};
#define NFILES 3

static int file; // index into files; NFILES done
static bool is_open, found_start;
static FIL fil;
static char chunk[CHUNK];
static char line[METLOG_LINE_MAX + 2];
static int line_len;
static bool skip_line; // the rest of a line we came into halfway, or one too long
static bool dev_chutil, dev_airtx; // device metrics had them
static uint32_t prev_up, prev_rx;  // the last local stats, for the packet rate
static bool have_prev;

void metview_clear(void) {
    memset(&want, 0, sizeof want);
    memset(&have, 0, sizeof have);
    loading = ready = is_open = false;
    start = newest = 0;
    memset(bins, 0, sizeof bins);
}

bool metview_ready(void) {
    return ready && have.node == want.node && have.span == want.span && have.end == want.end;
}
uint32_t metview_node(void) { return have.node; }
uint32_t metview_start(void) { return start; }
uint32_t metview_span(void) { return have.span; }
uint32_t metview_newest(void) { return newest; }

float metview_value(int i, mv_series_t s) {
    if (!ready || i < 0 || i >= METVIEW_BINS || !bins[i].n[s])
        return NAN;
    return bins[i].v[s];
}

uint32_t metview_time(int i) {
    return start + (uint32_t)(((uint64_t)have.span * (uint32_t)(2 * i + 1)) / (2 * METVIEW_BINS));
}

// --- loading ---------------------------------------------------------------------

static void begin(void) {
    have = want;
    start = have.end > have.span ? have.end - have.span : 0;
    newest = 0;
    memset(bins, 0, sizeof bins);
    file = 0;
    is_open = false;
    dev_chutil = dev_airtx = false;
    ready = false;
    loading = online;
    if (!online)
        ready = true; // nothing to read: the graphs go by RAM
}

static uint32_t stale_after(uint32_t span) {
    uint32_t s = span / 4;
    return s < STALE_MAX ? s : STALE_MAX;
}

void metview_want(uint32_t node, uint32_t span, uint32_t end) {
    if (!node || !span)
        return;
    if (node == want.node && span == want.span && end >= want.end &&
        end - want.end < stale_after(span))
        return;
    want = (want_t){node, span, end};
    begin();
}

void metview_set_online(bool on) {
    if (on == online)
        return;
    online = on;
    is_open = false; // gone with the card, or from before it
    if (want.node)
        begin();
}

bool metview_pending(void) { return online && loading; }

// "2026-10-11T15:30:00Z" to epoch seconds; 0 for anything else
static uint32_t parse_time(const char *s) {
    int y, mo, d, h, mi, se;
    char z;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%c", &y, &mo, &d, &h, &mi, &se, &z) != 7 || z != 'Z')
        return 0;
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31)
        return 0;
    // Days from 1970-01-01 (Howard Hinnant's days_from_civil)
    y -= mo <= 2;
    int era = y / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (unsigned)(mo + (mo > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    return (uint32_t)(days * 86400 + h * 3600 + mi * 60 + se);
}

static bool num(const char *s, float *v) {
    if (!s || !*s || (*s == '-' && !s[1]))
        return false;
    char *end;
    *v = strtof(s, &end);
    return end != s && isfinite(*v);
}

static void add(int b, mv_series_t s, float v) {
    if (bins[b].n[s] == 255)
        return;
    bins[b].v[s] += v;
    bins[b].n[s]++;
}

static void take_line(char *s) {
    char *f[FIELDS] = {0};
    int n = 0;
    for (char *p = s; n < FIELDS; p++) {
        f[n++] = p;
        p = strchr(p, '\t');
        if (!p)
            break;
        *p = 0;
    }
    if (n < 3)
        return;
    uint32_t t = parse_time(f[0]);
    if (!t || (strcmp(f[1], "answer") && strcmp(f[1], "heard")))
        return;
    met_kind_t kind = files[file];
    float v, up, rx, rxph = -1;
    // The packet rate, from the local stats before; those just before the
    // window count too
    if (kind == MET_LOCAL && n > 6 && num(f[2], &up) && num(f[6], &rx)) {
        if (have_prev && up >= (float)prev_up + 300 && rx >= (float)prev_rx)
            rxph = (rx - (float)prev_rx) * 3600 / (up - (float)prev_up);
        prev_up = (uint32_t)up, prev_rx = (uint32_t)rx, have_prev = true;
    }
    if (t < start || t >= have.end)
        return;
    int b = (int)((uint64_t)(t - start) * METVIEW_BINS / have.span);
    if (t > newest)
        newest = t;
    switch (kind) {
    case MET_DEVICE:
        if (n > 2 && num(f[2], &v) && v >= 0 && v <= 100)
            add(b, MV_BATT, v);
        if (n > 3 && num(f[3], &v) && v > 0)
            add(b, MV_MV, v);
        if (n > 4 && num(f[4], &v))
            add(b, MV_CHUTIL, v), dev_chutil = true;
        if (n > 5 && num(f[5], &v))
            add(b, MV_AIRTX, v), dev_airtx = true;
        break;
    case MET_ENV:
        if (num(f[2], &v))
            add(b, MV_TEMP, v);
        break;
    default:
        if (!dev_chutil && n > 3 && num(f[3], &v))
            add(b, MV_CHUTIL, v);
        if (!dev_airtx && n > 4 && num(f[4], &v))
            add(b, MV_AIRTX, v);
        if (n > 12 && num(f[12], &v))
            add(b, MV_ONLINE, v);
        if (n > 14 && num(f[14], &v) && v < 0)
            add(b, MV_NOISE, v);
        if (rxph >= 0)
            add(b, MV_RXPH, rxph);
        break;
    }
}

// The time on the first whole line after offset pos: 0 for none ("-"), or
// UINT32_MAX if there's no whole line there (the end of the file)
static FRESULT probe(FSIZE_t pos, uint32_t *t) {
    UINT got;
    FRESULT r = f_lseek(&fil, pos);
    if (r == FR_OK)
        r = f_read(&fil, chunk, CHUNK - 1, &got);
    if (r != FR_OK)
        return r;
    chunk[got] = 0;
    char *nl = strchr(chunk, '\n');
    *t = nl && strchr(nl + 1, '\n') ? parse_time(nl + 1) : UINT32_MAX;
    return FR_OK;
}

// Bisect the file for about where the window starts and go there. Lines
// with no time count as before it.
static FRESULT seek_start(void) {
    FSIZE_t lo = 0, hi = f_size(&fil);
    while (hi - lo > CHUNK) {
        FSIZE_t mid = lo + (hi - lo) / 2;
        uint32_t t;
        FRESULT r = probe(mid, &t);
        if (r != FR_OK)
            return r;
        if (t >= start)
            hi = mid;
        else
            lo = mid;
    }
    skip_line = lo != 0; // the header, or a part line
    line_len = 0;
    return f_lseek(&fil, lo);
}

static void finish(void) {
    for (int i = 0; i < METVIEW_BINS; i++)
        for (int s = 0; s < MV_SERIES; s++)
            if (bins[i].n[s])
                bins[i].v[s] /= bins[i].n[s];
    loading = false;
    ready = true;
}

int metview_task(void) {
    if (!metview_pending())
        return 0;
    FRESULT r = FR_OK;
    UINT budget = TASK_BYTES;
    while (file < NFILES && budget) {
        if (!is_open) {
            char path[sizeof METLOG_DIR + 24];
            snprintf(path, sizeof path, METLOG_DIR "/%08lx-%s.tsv", (unsigned long)have.node,
                     file_names[file]);
            r = f_open(&fil, path, FA_READ);
            if (r == FR_NO_FILE || r == FR_NO_PATH) {
                file++;
                r = FR_OK;
                continue;
            }
            if (r != FR_OK)
                break;
            is_open = true;
            found_start = false;
            have_prev = false;
            budget = budget > CHUNK * 4 ? budget - CHUNK * 4 : 0; // the search's reads, roughly
        }
        if (!found_start) {
            r = seek_start();
            if (r != FR_OK)
                break;
            found_start = true;
            continue;
        }
        UINT got;
        r = f_read(&fil, chunk, CHUNK, &got);
        if (r != FR_OK)
            break;
        budget = got < budget ? budget - got : 0;
        for (UINT i = 0; i < got; i++) {
            char c = chunk[i];
            if (c == '\n') {
                line[line_len] = 0;
                if (!skip_line && line_len)
                    take_line(line);
                skip_line = false;
                line_len = 0;
            } else if (line_len < (int)sizeof line - 1) {
                line[line_len++] = c;
            } else {
                skip_line = true;
            }
        }
        if (got < CHUNK) { // the end of the file
            f_close(&fil);
            is_open = false;
            file++;
        }
    }
    if (r != FR_OK) {
        printf("[sd] can't read the metrics log (FatFs error %d)\n", r);
        is_open = false;
        return -1;
    }
    if (file >= NFILES)
        finish();
    return 0;
}
