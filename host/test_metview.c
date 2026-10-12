// The graphs' history read back from the metrics log (src/sd/metview.c), on
// a FAT-formatted RAM disk, with log files written by metlog.c.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "metrics.h"
#include "ramdisk.h"
#include "sd/metlog.h"
#include "sd/metview.h"

static int failures;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
            failures++;                                                        \
        }                                                                      \
    } while (0)

#define NODE 0x0badf00du
#define NOW 1791936000u // 2026-10-14T00:00:00Z
#define DAYS 10

static FATFS fs;

static void flush(void) {
    while (metlog_pending())
        CHECK(metlog_flush() == 0);
}

// Volts falling by a millivolt a minute, channel use the hour of day
static float volts_at(uint32_t t) { return 4.2f - (float)(NOW - t) / 60 * 0.0001f; }

static void write_log(void) {
    mesh_metrics_t e;
    memset(&e, 0, sizeof e);
    e.num = NODE;
    for (uint32_t t = NOW - DAYS * 86400; t < NOW; t += 600) {
        meshtastic_DeviceMetrics *d = &e.device;
        memset(d, 0, sizeof *d);
        d->has_voltage = d->has_battery_level = d->has_channel_utilization = true;
        d->voltage = volts_at(t);
        d->battery_level = 101; // powered: no percent graph
        d->channel_utilization = (float)(t % 86400 / 3600);
        metlog_telemetry(&e, MET_DEVICE, t, false);
        if (t % 3600 == 0) {
            meshtastic_LocalStats *l = &e.local;
            memset(l, 0, sizeof *l);
            l->uptime_seconds = t - (NOW - DAYS * 86400);
            l->num_packets_rx = l->uptime_seconds / 36; // 100 an hour
            l->num_online_nodes = 7;
            l->noise_floor = -110;
            metlog_telemetry(&e, MET_LOCAL, t, true);
        }
        flush();
    }
    // A run that got nothing, and a line from before the node had the time
    metlog_failed(NODE, MET_DEVICE, MET_ERR_NO_ANSWER, NOW - 300);
    metlog_telemetry(&e, MET_DEVICE, 0, false);
    flush();
}

static int load(uint32_t node, uint32_t span, uint32_t end) {
    metview_want(node, span, end);
    int calls = 0;
    while (metview_pending()) {
        CHECK(metview_task() == 0);
        if (++calls > 10000)
            break;
    }
    return calls;
}

static int bin_of(uint32_t t) {
    return (int)((uint64_t)(t - metview_start()) * METVIEW_BINS / metview_span());
}

static void test_day(void) {
    int calls = load(NODE, 86400, NOW);
    CHECK(metview_ready() && metview_node() == NODE && metview_start() == NOW - 86400);
    CHECK(calls > 1); // a bit at a time, not ten days in one go
    CHECK(calls < 20); // ... and not all ten days: it searched for the start
    CHECK(metview_newest() == NOW - 600);

    // Each slot is the mean of the lines in it
    int b = bin_of(NOW - 3600);
    float sum = 0;
    int n = 0;
    for (uint32_t t = NOW - 86400; t < NOW; t += 600)
        if (bin_of(t) == b)
            sum += volts_at(t), n++;
    CHECK(n > 0 && fabsf(metview_value(b, MV_MV) - sum / (float)n) < 1e-4f);
    CHECK(isnan(metview_value(b, MV_BATT)));
    // One line every ten minutes, a slot 11.25 minutes: none is empty
    int empty = 0;
    for (int i = 0; i < METVIEW_BINS; i++)
        empty += isnan(metview_value(i, MV_MV));
    CHECK(empty < METVIEW_BINS / 8);
    CHECK(fabsf(metview_value(0, MV_MV) - volts_at(NOW - 86400)) < 0.002f);

    // Local stats: once an hour, and from the second one in the file on, a
    // packet rate (the first in the window has the one before it)
    int got = 0, rate = 0;
    for (int i = 0; i < METVIEW_BINS; i++) {
        float v = metview_value(i, MV_ONLINE);
        if (!isnan(v)) {
            got++;
            CHECK(v == 7 && metview_value(i, MV_NOISE) == -110);
            float r = metview_value(i, MV_RXPH);
            rate += !isnan(r);
            CHECK(isnan(r) || fabsf(r - 100) < 0.5f);
        }
    }
    CHECK(got == 24 && rate == 24);
    // Channel use comes from device metrics; local stats had none to add
    CHECK(fabsf(metview_value(bin_of(NOW - 600), MV_CHUTIL) - 23) < 0.01f);
}

static void test_week(void) {
    load(NODE, 7 * 86400, NOW);
    CHECK(metview_ready() && metview_span() == 7 * 86400);
    CHECK(!isnan(metview_value(0, MV_MV)) && !isnan(metview_value(METVIEW_BINS - 1, MV_MV)));
    // Window ends before the log does: nothing after it
    load(NODE, 3 * 3600, NOW - 2 * 86400);
    CHECK(metview_newest() < NOW - 2 * 86400 && metview_newest() >= NOW - 2 * 86400 - 600);
}

static void test_reload(void) {
    load(NODE, 3 * 3600, NOW);
    // The end moving on a little: still the same load
    metview_want(NODE, 3 * 3600, NOW + 600);
    CHECK(metview_ready() && !metview_pending());
    // A quarter of the window on: loaded again
    metview_want(NODE, 3 * 3600, NOW + 2700);
    CHECK(!metview_ready() && metview_pending());
    load(NODE, 3 * 3600, NOW + 2700);
    CHECK(metview_ready() && metview_start() == NOW + 2700 - 3 * 3600);
    // Another node, with nothing on the card
    load(0x12345678, 86400, NOW);
    CHECK(metview_ready() && metview_newest() == 0 && isnan(metview_value(5, MV_MV)));
}

static void test_no_card(void) {
    metview_set_online(false);
    metview_want(NODE, 12 * 3600, NOW);
    CHECK(metview_ready() && !metview_pending() && isnan(metview_value(60, MV_MV)));
    // The card back: loaded from it
    metview_set_online(true);
    CHECK(!metview_ready() && metview_pending());
    load(NODE, 12 * 3600, NOW);
    CHECK(metview_ready() && !isnan(metview_value(60, MV_MV)));
}

int main(void) {
    static BYTE work[FF_MAX_SS];
    ramdisk_create(8u << 20);
    MKFS_PARM opt = {.fmt = FM_FAT | FM_SFD};
    CHECK(f_mkfs("", &opt, work, sizeof work) == FR_OK);
    CHECK(f_mount(&fs, "", 1) == FR_OK);
    metlog_set_online(true);
    metview_set_online(true);
    write_log();
    test_day();
    test_week();
    test_reload();
    test_no_card();
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    return failures != 0;
}
