#include "metrics.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "link.h"
#include "nodedb.h"

static mesh_metrics_t table[MESH_METRICS_NODES];
static int count;
static met_add_cb on_add;

void mesh_metrics_on_add(met_add_cb cb) { on_add = cb; }

void mesh_metrics_clear(void) {
    memset(table, 0, sizeof table);
    count = 0;
}

const mesh_metrics_t *mesh_metrics_find(uint32_t num) {
    for (int i = 0; i < count; i++)
        if (table[i].num == num)
            return &table[i];
    return NULL;
}

int mesh_metrics_count(void) { return count; }
const mesh_metrics_t *mesh_metrics_at(int i) { return i >= 0 && i < count ? &table[i] : NULL; }

static uint32_t newest(const mesh_metrics_t *e) {
    uint32_t t = 0;
    for (int k = 0; k < MET_KINDS; k++)
        if (e->time[k] > t)
            t = e->time[k];
    return t;
}

// The node's entry, made if need be: when the table is full, the node heard
// from longest ago (never our own) makes room.
static mesh_metrics_t *entry(uint32_t num) {
    mesh_metrics_t *e = (mesh_metrics_t *)mesh_metrics_find(num);
    if (e)
        return e;
    if (count < MESH_METRICS_NODES) {
        e = &table[count++];
    } else {
        for (int i = 0; i < count; i++) {
            if (table[i].num == mesh_db.my_node_num && mesh_db.my_node_num)
                continue;
            if (!e || newest(&table[i]) < newest(e))
                e = &table[i];
        }
    }
    memset(e, 0, sizeof *e);
    e->num = num;
    return e;
}

const met_point_t *mesh_metrics_point(const mesh_metrics_t *e, int i) {
    return &e->hist[(e->hist_first + i) % MESH_METRICS_HIST];
}

static met_point_t *last_point(mesh_metrics_t *e) {
    return e->nhist ? (met_point_t *)mesh_metrics_point(e, e->nhist - 1) : NULL;
}

// Whether the point has no numbers from this kind of packet yet
static bool point_free(const met_point_t *p, met_kind_t kind) {
    switch (kind) {
    // (local stats may have put in channel use: device metrics replace it)
    case MET_DEVICE: return !p->mv && p->batt == MET_NONE_U8;
    case MET_ENV: return p->temp == MET_NONE_TEMP;
    default: return p->online == MET_NONE_U16 && p->rxph == MET_NONE_U16 && !p->noise;
    }
}

// The point to put this packet's numbers in: the last one if it's recent and
// doesn't have them yet (device, environment and local stats packets come
// close together), else a new one, at most every MESH_METRICS_GAP_S.
static met_point_t *point_for(mesh_metrics_t *e, uint32_t time, met_kind_t kind) {
    if (!time)
        return NULL;
    met_point_t *p = last_point(e);
    if (p && time >= p->time && time - p->time < MESH_METRICS_GAP_S)
        return point_free(p, kind) ? p : NULL;
    if (p && time < p->time)
        return NULL; // the node's clock went back (a reboot without a fix): skip
    if (e->nhist == MESH_METRICS_HIST) {
        e->hist_first = (uint8_t)((e->hist_first + 1) % MESH_METRICS_HIST);
        e->nhist--;
    }
    p = &e->hist[(e->hist_first + e->nhist++) % MESH_METRICS_HIST];
    p->time = time;
    p->mv = 0;
    p->batt = p->chutil = MET_NONE_U8;
    p->temp = MET_NONE_TEMP;
    p->airtx = p->online = p->rxph = MET_NONE_U16;
    p->noise = 0;
    return p;
}

static uint8_t clamp_u8(float v, float max) {
    if (!(v >= 0))
        return 0;
    return (uint8_t)(v > max ? max : v + 0.5f);
}

static uint16_t clamp_u16(float v, float max) {
    if (!(v >= 0))
        return 0;
    return (uint16_t)(v > max ? max : v + 0.5f);
}

static void add_device(mesh_metrics_t *e, uint32_t time) {
    const meshtastic_DeviceMetrics *d = &e->device;
    met_point_t *p = point_for(e, time, MET_DEVICE);
    if (p) {
        if (d->has_voltage && d->voltage > 0)
            p->mv = (uint16_t)(d->voltage > 65 ? 65000 : d->voltage * 1000 + 0.5f);
        if (d->has_battery_level)
            p->batt = clamp_u8((float)d->battery_level, 101);
        if (d->has_channel_utilization)
            p->chutil = clamp_u8(d->channel_utilization * 2, 200);
        if (d->has_air_util_tx)
            p->airtx = clamp_u16(d->air_util_tx * 100, 10000);
    }
    // Keep the Nodes tab's battery column current too
    mesh_node_t *n = mesh_db_find(e->num);
    if (n && d->has_battery_level)
        n->battery = d->battery_level > MESH_BATTERY_POWERED ? MESH_BATTERY_POWERED
                                                             : (uint8_t)d->battery_level;
}

static void add_env(mesh_metrics_t *e, uint32_t time) {
    if (!e->env.has_temperature || !isfinite(e->env.temperature))
        return;
    met_point_t *p = point_for(e, time, MET_ENV);
    float t = e->env.temperature * 10;
    if (p && t > -3000 && t < 3000)
        p->temp = (int16_t)(t < 0 ? t - 0.5f : t + 0.5f);
}

// prev: the node's last local stats, if any, for the packet rate
static void add_local(mesh_metrics_t *e, const meshtastic_LocalStats *prev, uint32_t time) {
    const meshtastic_LocalStats *l = &e->local;
    met_point_t *p = point_for(e, time, MET_LOCAL);
    if (!p)
        return;
    p->online = (uint16_t)(l->num_online_nodes < 0xFFFE ? l->num_online_nodes : 0xFFFE);
    if (l->noise_floor < 0)
        p->noise = (int8_t)(l->noise_floor < -127 ? -127 : l->noise_floor);
    // Its counters since the last ones; not across a reboot (they restart),
    // nor over a few minutes, which says little
    if (prev && l->uptime_seconds >= prev->uptime_seconds + 300 &&
        l->num_packets_rx >= prev->num_packets_rx) {
        uint64_t n = (uint64_t)(l->num_packets_rx - prev->num_packets_rx) * 3600 /
                     (l->uptime_seconds - prev->uptime_seconds);
        p->rxph = (uint16_t)(n < 0xFFFE ? n : 0xFFFE);
    }
    if (p->chutil == MET_NONE_U8)
        p->chutil = clamp_u8(l->channel_utilization * 2, 200);
    if (p->airtx == MET_NONE_U16)
        p->airtx = clamp_u16(l->air_util_tx * 100, 10000);
}

mesh_metrics_t *mesh_metrics_add(uint32_t num, const meshtastic_Telemetry *t, uint32_t time) {
    met_kind_t kind;
    switch (t->which_variant) {
    case meshtastic_Telemetry_device_metrics_tag: kind = MET_DEVICE; break;
    case meshtastic_Telemetry_environment_metrics_tag: kind = MET_ENV; break;
    case meshtastic_Telemetry_power_metrics_tag: kind = MET_POWER; break;
    case meshtastic_Telemetry_local_stats_tag: kind = MET_LOCAL; break;
    default: return NULL;
    }
    if (!time)
        time = t->time;
    mesh_metrics_t *e = entry(num);
    // Our own node's clock can be unset until it hears one; keep the numbers
    // even so, with the time we know
    bool had_local = e->have[MET_LOCAL];
    e->time[kind] = time;
    e->have[kind] = true;
    switch (kind) {
    case MET_DEVICE:
        e->device = t->variant.device_metrics;
        add_device(e, time);
        break;
    case MET_ENV:
        e->env = t->variant.environment_metrics;
        add_env(e, time);
        break;
    case MET_POWER:
        e->power = t->variant.power_metrics;
        break;
    default: {
        meshtastic_LocalStats prev = e->local;
        e->local = t->variant.local_stats;
        add_local(e, had_local ? &prev : NULL, time);
        break;
    }
    }
    if (on_add)
        on_add(e, kind, time);
    return e;
}

void mesh_metrics_nodeinfo(uint32_t num, const meshtastic_DeviceMetrics *dm, uint32_t time) {
    const mesh_metrics_t *have = mesh_metrics_find(num);
    if (have && have->have[MET_DEVICE] && have->time[MET_DEVICE] >= time)
        return;
    // Only battery is worth a slot of its own; the Nodes tab has that already
    if (!have && !dm->has_voltage && !dm->has_channel_utilization)
        return;
    mesh_metrics_t *e = entry(num);
    e->device = *dm;
    e->time[MET_DEVICE] = time;
    e->have[MET_DEVICE] = true;
}

// --- requests -------------------------------------------------------------------

static met_req_state_t req_state;
static uint32_t req_node, req_id, req_ms, req_wait, req_seq;
static met_kind_t req_kind;
static uint8_t req_error;

bool mesh_metrics_request(uint32_t node, met_kind_t kind) {
    return mesh_metrics_request_wait(
        node, kind, node == mesh_db.my_node_num ? MET_REQ_LOCAL_MS : MET_REQ_REMOTE_MS);
}

bool mesh_metrics_request_wait(uint32_t node, met_kind_t kind, uint32_t wait_ms) {
    static const pb_size_t tags[MET_KINDS] = {
        [MET_DEVICE] = meshtastic_Telemetry_device_metrics_tag,
        [MET_ENV] = meshtastic_Telemetry_environment_metrics_tag,
        [MET_POWER] = meshtastic_Telemetry_power_metrics_tag,
        [MET_LOCAL] = meshtastic_Telemetry_local_stats_tag,
    };
    if ((unsigned)kind >= MET_KINDS)
        return false;
    req_node = node;
    req_kind = kind;
    req_ms = mesh_link_ms();
    req_wait = wait_ms;
    req_seq++;
    if (!mesh_link_send_telemetry_req(node, tags[kind], &req_id)) {
        req_state = MET_REQ_FAILED;
        req_error = MET_ERR_NO_LINK;
        return false;
    }
    req_state = MET_REQ_WAITING;
    req_error = 0;
    return true;
}

void mesh_metrics_poll(void) {
    if (req_state != MET_REQ_WAITING)
        return;
    if (mesh_link_ms() - req_ms >= req_wait) {
        req_state = MET_REQ_FAILED;
        req_error = MET_ERR_NO_ANSWER;
    }
}

void mesh_metrics_answer(uint32_t request_id, uint32_t from) {
    (void)from;
    if (req_state == MET_REQ_WAITING && request_id && request_id == req_id)
        req_state = MET_REQ_DONE;
}

void mesh_metrics_routing(uint32_t request_id, uint32_t from, uint8_t error) {
    (void)from;
    // An ack only says it got there; the answer is what we wait for
    if (req_state == MET_REQ_WAITING && request_id == req_id && error) {
        req_state = MET_REQ_FAILED;
        req_error = error;
    }
}

met_req_state_t mesh_metrics_req_state(void) { return req_state; }
uint32_t mesh_metrics_req_node(void) { return req_node; }
met_kind_t mesh_metrics_req_kind(void) { return req_kind; }
uint32_t mesh_metrics_req_age_ms(void) { return mesh_link_ms() - req_ms; }
uint8_t mesh_metrics_req_error(void) { return req_error; }
uint32_t mesh_metrics_req_seq(void) { return req_seq; }

const char *mesh_metrics_kind_name(met_kind_t k) {
    static const char *const names[MET_KINDS] = {
        [MET_DEVICE] = "device metrics",
        [MET_ENV] = "environment metrics",
        [MET_POWER] = "power metrics",
        [MET_LOCAL] = "local stats",
    };
    return (unsigned)k < MET_KINDS ? names[k] : "?";
}

// --- console --------------------------------------------------------------------

#define PF(has, name, fmt, v)                                                  \
    do {                                                                       \
        if (has)                                                               \
            printf("  %-14s " fmt "\n", name, v);                             \
    } while (0)

void mesh_metrics_dump(uint32_t num) {
    const mesh_metrics_t *e = mesh_metrics_find(num);
    if (!e) {
        printf("no metrics from !%08lx\n", (unsigned long)num);
        return;
    }
    printf("metrics of !%08lx, %u history points\n", (unsigned long)num, e->nhist);
    if (e->have[MET_DEVICE]) {
        const meshtastic_DeviceMetrics *d = &e->device;
        printf(" device (at %lu)\n", (unsigned long)e->time[MET_DEVICE]);
        PF(d->has_battery_level, "battery", "%lu%%", (unsigned long)d->battery_level);
        PF(d->has_voltage, "voltage", "%.2f V", (double)d->voltage);
        PF(d->has_channel_utilization, "channel util", "%.1f%%", (double)d->channel_utilization);
        PF(d->has_air_util_tx, "air util tx", "%.2f%%", (double)d->air_util_tx);
        PF(d->has_uptime_seconds, "uptime", "%lu s", (unsigned long)d->uptime_seconds);
    }
    if (e->have[MET_ENV]) {
        const meshtastic_EnvironmentMetrics *v = &e->env;
        printf(" environment (at %lu)\n", (unsigned long)e->time[MET_ENV]);
        PF(v->has_temperature, "temperature", "%.1f C", (double)v->temperature);
        PF(v->has_relative_humidity, "humidity", "%.0f%%", (double)v->relative_humidity);
        PF(v->has_barometric_pressure, "pressure", "%.1f hPa", (double)v->barometric_pressure);
        PF(v->has_gas_resistance, "gas", "%.1f MOhm", (double)v->gas_resistance);
        PF(v->has_iaq, "IAQ", "%u", (unsigned)v->iaq);
        PF(v->has_lux, "light", "%.0f lx", (double)v->lux);
        PF(v->has_voltage, "voltage", "%.2f V", (double)v->voltage);
        PF(v->has_current, "current", "%.1f mA", (double)v->current);
    }
    if (e->have[MET_POWER]) {
        const meshtastic_PowerMetrics *w = &e->power;
        printf(" power (at %lu)\n", (unsigned long)e->time[MET_POWER]);
        PF(w->has_ch1_voltage, "ch1", "%.2f V", (double)w->ch1_voltage);
        PF(w->has_ch1_current, "ch1", "%.1f mA", (double)w->ch1_current);
        PF(w->has_ch2_voltage, "ch2", "%.2f V", (double)w->ch2_voltage);
        PF(w->has_ch2_current, "ch2", "%.1f mA", (double)w->ch2_current);
        PF(w->has_ch3_voltage, "ch3", "%.2f V", (double)w->ch3_voltage);
        PF(w->has_ch3_current, "ch3", "%.1f mA", (double)w->ch3_current);
    }
    if (e->have[MET_LOCAL]) {
        const meshtastic_LocalStats *l = &e->local;
        printf(" local stats (at %lu)\n", (unsigned long)e->time[MET_LOCAL]);
        printf("  uptime %lu s, chutil %.1f%%, airtx %.2f%%\n", (unsigned long)l->uptime_seconds,
               (double)l->channel_utilization, (double)l->air_util_tx);
        printf("  packets tx %lu rx %lu bad %lu dupe %lu\n", (unsigned long)l->num_packets_tx,
               (unsigned long)l->num_packets_rx, (unsigned long)l->num_packets_rx_bad,
               (unsigned long)l->num_rx_dupe);
        printf("  relayed %lu cancelled %lu dropped %u\n", (unsigned long)l->num_tx_relay,
               (unsigned long)l->num_tx_relay_canceled, (unsigned)l->num_tx_dropped);
        printf("  nodes online %u of %u, noise floor %ld dBm, heap %lu/%lu free\n",
               (unsigned)l->num_online_nodes, (unsigned)l->num_total_nodes,
               (long)l->noise_floor, (unsigned long)l->heap_free_bytes,
               (unsigned long)l->heap_total_bytes);
    }
}
