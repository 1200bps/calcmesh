#include "sd/metlog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ff.h"

#define QUEUE 12 // lines

typedef struct {
    uint32_t node;
    met_kind_t kind;
    char line[METLOG_LINE_MAX + 1];
} rec_t;

static rec_t queue[QUEUE];
static int q_first, q_count;
static bool online;
static metlog_stats_t stats;
static FIL fil;

static const char *const kind_file[MET_KINDS] = {
    [MET_DEVICE] = "device", [MET_ENV] = "env", [MET_POWER] = "power", [MET_LOCAL] = "local"};

static const char *const headers[MET_KINDS] = {
    [MET_DEVICE] = "time\tgot\tbattery\tvolts\tchutil\tairtx\tuptime\n",
    [MET_ENV] = "time\tgot\ttemp\thumidity\tpressure\tgas\tiaq\tlux\twind\twind_dir\tgust"
                "\train_1h\train_24h\tvolts\tcurrent\n",
    [MET_POWER] = "time\tgot\tch1_v\tch1_ma\tch2_v\tch2_ma\tch3_v\tch3_ma\n",
    [MET_LOCAL] = "time\tgot\tuptime\tchutil\tairtx\ttx\trx\trx_bad\trx_dupe\trelayed"
                  "\trelay_cancel\ttx_dropped\tonline\tnodes\tnoise\theap_free\n",
};
static const int columns[MET_KINDS] = {
    [MET_DEVICE] = 7, [MET_ENV] = 15, [MET_POWER] = 8, [MET_LOCAL] = 16};

void metlog_clear(void) {
    q_first = q_count = 0;
    memset(&stats, 0, sizeof stats);
}

void metlog_set_online(bool on) { online = on; }
bool metlog_pending(void) { return online && q_count; }
const metlog_stats_t *metlog_stats(void) { return &stats; }

// --- formatting --------------------------------------------------------------------

typedef struct {
    char *p, *end;
} out_t;

static void put(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(out_t *o, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->p, (size_t)(o->end - o->p), fmt, ap);
    va_end(ap);
    if (n > 0)
        o->p += n < o->end - o->p ? n : o->end - o->p - 1;
}

static void put_f(out_t *o, bool has, const char *fmt, float v) {
    put(o, "\t");
    if (has)
        put(o, fmt, (double)v);
    else
        put(o, "-");
}

static void put_u(out_t *o, bool has, uint32_t v) {
    if (has)
        put(o, "\t%lu", (unsigned long)v);
    else
        put(o, "\t-");
}

static void put_time(out_t *o, uint32_t epoch) {
    if (!epoch) {
        put(o, "-");
        return;
    }
    time_t t = (time_t)epoch;
    struct tm tm;
    gmtime_r(&t, &tm);
    put(o, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static rec_t *push(uint32_t node, met_kind_t kind) {
    if ((unsigned)kind >= MET_KINDS)
        return NULL;
    if (q_count == QUEUE) {
        stats.dropped++;
        return NULL;
    }
    rec_t *r = &queue[(q_first + q_count++) % QUEUE];
    r->node = node;
    r->kind = kind;
    return r;
}

void metlog_telemetry(const mesh_metrics_t *e, met_kind_t kind, uint32_t time, bool asked) {
    rec_t *r = push(e->num, kind);
    if (!r)
        return;
    out_t o = {r->line, r->line + sizeof r->line - 1};
    put_time(&o, time);
    put(&o, "\t%s", asked ? "answer" : "heard");
    switch (kind) {
    case MET_DEVICE: {
        const meshtastic_DeviceMetrics *d = &e->device;
        put_u(&o, d->has_battery_level, d->battery_level);
        put_f(&o, d->has_voltage, "%.3f", d->voltage);
        put_f(&o, d->has_channel_utilization, "%.2f", d->channel_utilization);
        put_f(&o, d->has_air_util_tx, "%.3f", d->air_util_tx);
        put_u(&o, d->has_uptime_seconds, d->uptime_seconds);
        break;
    }
    case MET_ENV: {
        const meshtastic_EnvironmentMetrics *v = &e->env;
        put_f(&o, v->has_temperature, "%.2f", v->temperature);
        put_f(&o, v->has_relative_humidity, "%.1f", v->relative_humidity);
        put_f(&o, v->has_barometric_pressure, "%.2f", v->barometric_pressure);
        put_f(&o, v->has_gas_resistance, "%.3f", v->gas_resistance);
        put_u(&o, v->has_iaq, v->iaq);
        put_f(&o, v->has_lux, "%.1f", v->lux);
        put_f(&o, v->has_wind_speed, "%.1f", v->wind_speed);
        put_u(&o, v->has_wind_direction, v->wind_direction);
        put_f(&o, v->has_wind_gust, "%.1f", v->wind_gust);
        put_f(&o, v->has_rainfall_1h, "%.1f", v->rainfall_1h);
        put_f(&o, v->has_rainfall_24h, "%.1f", v->rainfall_24h);
        put_f(&o, v->has_voltage, "%.3f", v->voltage);
        put_f(&o, v->has_current, "%.1f", v->current);
        break;
    }
    case MET_POWER: {
        const meshtastic_PowerMetrics *w = &e->power;
        put_f(&o, w->has_ch1_voltage, "%.3f", w->ch1_voltage);
        put_f(&o, w->has_ch1_current, "%.1f", w->ch1_current);
        put_f(&o, w->has_ch2_voltage, "%.3f", w->ch2_voltage);
        put_f(&o, w->has_ch2_current, "%.1f", w->ch2_current);
        put_f(&o, w->has_ch3_voltage, "%.3f", w->ch3_voltage);
        put_f(&o, w->has_ch3_current, "%.1f", w->ch3_current);
        break;
    }
    default: {
        const meshtastic_LocalStats *l = &e->local;
        put(&o, "\t%lu\t%.2f\t%.3f\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%u\t%u\t%u",
            (unsigned long)l->uptime_seconds, (double)l->channel_utilization,
            (double)l->air_util_tx, (unsigned long)l->num_packets_tx,
            (unsigned long)l->num_packets_rx, (unsigned long)l->num_packets_rx_bad,
            (unsigned long)l->num_rx_dupe, (unsigned long)l->num_tx_relay,
            (unsigned long)l->num_tx_relay_canceled, (unsigned)l->num_tx_dropped,
            (unsigned)l->num_online_nodes, (unsigned)l->num_total_nodes);
        if (l->noise_floor)
            put(&o, "\t%ld", (long)l->noise_floor);
        else
            put(&o, "\t-");
        put_u(&o, l->heap_total_bytes != 0, l->heap_free_bytes);
        break;
    }
    }
    put(&o, "\n");
}

void metlog_failed(uint32_t node, met_kind_t kind, uint8_t error, uint32_t time) {
    rec_t *r = push(node, kind);
    if (!r)
        return;
    out_t o = {r->line, r->line + sizeof r->line - 1};
    put_time(&o, time);
    if (error == MET_ERR_NO_ANSWER)
        put(&o, "\tno-answer");
    else
        put(&o, "\terror-%u", (unsigned)error);
    for (int i = 2; i < columns[kind]; i++)
        put(&o, "\t-");
    put(&o, "\n");
}

// --- the card ----------------------------------------------------------------------

static FRESULT open_file(const char *path) {
    FRESULT r = f_open(&fil, path, FA_OPEN_APPEND | FA_WRITE);
    if (r != FR_NO_PATH)
        return r;
    r = f_mkdir("/calcmesh");
    if (r != FR_OK && r != FR_EXIST)
        return r;
    r = f_mkdir(METLOG_DIR);
    if (r != FR_OK && r != FR_EXIST)
        return r;
    return f_open(&fil, path, FA_OPEN_APPEND | FA_WRITE);
}

static FRESULT write_str(const char *s) {
    UINT n, len = (UINT)strlen(s);
    FRESULT r = f_write(&fil, s, len, &n);
    return r == FR_OK && n != len ? FR_DENIED : r; // short: the card is full
}

int metlog_flush(void) {
    if (!online || !q_count)
        return 0;
    rec_t *rec = &queue[q_first];
    char path[sizeof METLOG_DIR + 24];
    snprintf(path, sizeof path, METLOG_DIR "/%08lx-%s.tsv", (unsigned long)rec->node,
             kind_file[rec->kind]);
    FRESULT r = open_file(path);
    if (r == FR_OK) {
        if (f_size(&fil) == 0)
            r = write_str(headers[rec->kind]);
        if (r == FR_OK)
            r = write_str(rec->line);
        FRESULT c = f_close(&fil);
        if (r == FR_OK)
            r = c;
    }
    if (r != FR_OK) {
        stats.errors++;
        printf("[sd] can't write %s (FatFs error %d)\n", path, r);
        return -1;
    }
    q_first = (q_first + 1) % QUEUE;
    q_count--;
    stats.written++;
    return 0;
}
