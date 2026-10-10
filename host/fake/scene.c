#include "scene.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mesh/metrics.h"
#include "mesh/nodeedit.h"
#include "msgs.h"
#include "nodedb.h"
#include "sd/settings.h"
#include "ui.h"

static const struct {
    const char *lname, *sname;
    int hops;   // -1 unknown
    float snr;
    int ago;    // seconds since heard, -1 never
    int batt;   // -1 unknown, 101 powered
    bool fav;
} nodes[] = {
    {"Ridge Repeater", "RDG1", 0, 9.25f, 40, 101, true},
    {"Juniper Base", "JUNI", 0, -3.5f, 95, 87, false},
    {"K7 Mobile", "K7MB", 1, 0, 300, 64, false},
    {"Hilltop Solar", "HTS", 0, -11.75f, 610, 42, true},
    {"Corvid", "CRVD", 2, 0, 1500, 15, false},
    {"Library Roof", "LIB", 1, 0, 2400, 101, true},
    {"Tbeam Trail", "TBT", 3, 0, 4000, 91, false},
    {"Pocket Rak", "PRAK", 2, 0, 7300, 55, false},
    {"Wren", "WREN", 0, 2.0f, 9000, 77, false},
    {"Meshtastic 5e1f", "5e1f", -1, 0, 20000, -1, false},
    {"Northside Relay", "NSR", 4, 0, 40000, 101, false},
    {"Kayak Node", "KYK", 5, 0, 90000, 33, false},
    {"Cabin", "CBN", 2, 0, 200000, 100, false},
    {"Desert Ham", "DSRT", 3, 0, 400000, 8, false},
    {"Old Tracker", "OLDT", -1, 0, -1, -1, false},
    {"Bridge Watch", "BRDG", 1, 0, 3100, 69, false},
    {"Fire Lookout 4", "FL4", 3, 0, 14000, 101, false},
    {"Café Node ñ", "CAFE", 1, 0, 5000, 50, false},
    {"Rooftop West", "RFW", 2, 0, 18000, 81, false},
    {"Sailboat", "SAIL", 4, 0, 30000, 72, false},
    {"Dog Park", "DOG", 1, 0, 6000, 93, false},
    {"Weather Stn", "WX", 2, 0, 1200, 101, false},
    {"Lighthouse", "LTHS", 3, 0, 50000, 101, false},
    {"Basement Lab", "LAB", 0, 6.5f, 150, 101, false},
    {"Rover 2", "RVR2", 2, 0, 75000, 44, false},
    {"Quiet One", "QT", 5, 0, 500000, 12, false},
};

#define NNODES (int)(sizeof nodes / sizeof nodes[0])

// Made-up positions: km north and east of us, for the nodes that share one
// (the rest stay unplaced). Desert Ham is far enough off to sit outside a
// fitted view's neighbours; Sailboat is out on the water.
#define SCENE_LAT 455230000 // 45.523 N, 122.676 W
#define SCENE_LON -1226760000
static const struct {
    int node;
    float north, east;
    int alt;
} places[] = {
    {0, 6.8f, -9.4f, 610},  {1, -1.2f, 2.1f, 48},    {2, 3.5f, 5.9f, 0},
    {3, 9.6f, 4.2f, 402},   {4, -7.4f, -3.3f, 95},   {5, 0.6f, -0.9f, 61},
    {6, -10.8f, 11.5f, 870}, {8, 2.2f, -4.6f, 2},    {11, 13.9f, -15.2f, 3},
    {12, -16.0f, 19.0f, 1150}, {13, -95.0f, 160.0f, 1310}, {15, 1.4f, 1.0f, 30},
    {16, 18.5f, 22.0f, 1640}, {17, -0.4f, 0.3f, 52}, {19, 5.1f, -12.8f, 0},
    {20, -2.9f, -1.6f, 70}, {21, 4.4f, 0.8f, 120},   {23, 0.1f, -0.2f, 45},
};

static void place(uint32_t num, float north, float east, int alt) {
    meshtastic_Position pos = meshtastic_Position_init_zero;
    pos.has_latitude_i = pos.has_longitude_i = pos.has_altitude = true;
    pos.latitude_i = SCENE_LAT + (int32_t)(north * 1e7f / 111.195f);
    pos.longitude_i = SCENE_LON + (int32_t)(east * 1e7f / (111.195f * 0.70130f));
    pos.altitude = alt;
    pos.precision_bits = 32;
    mesh_db_set_position(num, &pos);
}

uint32_t scene_node(int i) { return 0x10000000u + (uint32_t)i * 0x01010101u; }

static void add_node(uint32_t num, const char *lname, const char *sname,
                     int hops, float snr, int ago, int batt, bool fav) {
    meshtastic_NodeInfo ni = meshtastic_NodeInfo_init_zero;
    ni.num = num;
    ni.has_user = true;
    snprintf(ni.user.id, sizeof ni.user.id, "!%08x", (unsigned)num);
    snprintf(ni.user.long_name, sizeof ni.user.long_name, "%s", lname);
    snprintf(ni.user.short_name, sizeof ni.user.short_name, "%s", sname);
    if (hops >= 0) {
        ni.has_hops_away = true;
        ni.hops_away = (uint32_t)hops;
    }
    ni.snr = snr;
    ni.last_heard = ago < 0 ? 0 : SCENE_NOW - (uint32_t)ago;
    if (batt >= 0) {
        ni.has_device_metrics = true;
        ni.device_metrics.has_battery_level = true;
        ni.device_metrics.battery_level = (uint32_t)batt;
    }
    ni.is_favorite = fav;
    // Most have sent their public key; a few old or quiet ones haven't
    if (ago >= 0 && hops >= 0 && hops < 5) {
        ni.user.public_key.size = 32;
        for (int i = 0; i < 32; i++)
            ni.user.public_key.bytes[i] = (uint8_t)(num >> (i % 4 * 8)) ^ (uint8_t)(i * 37);
    }
    mesh_db_upsert_node(&ni);
    if (num == SCENE_ME) {
        mesh_db.owner = ni.user;
        mesh_db.has_owner = true;
    }
}

static void add_channel(int index, int role, const char *name) {
    meshtastic_Channel ch = meshtastic_Channel_init_zero;
    ch.index = index;
    ch.role = role;
    ch.has_settings = true;
    snprintf(ch.settings.name, sizeof ch.settings.name, "%s", name);
    ch.settings.psk.size = 1;
    ch.settings.psk.bytes[0] = 1;
    mesh_db_set_channel(&ch);
}

const mesh_msg_t *scene_msg(uint32_t from, mesh_conv_t conv, uint32_t time,
                            uint8_t status, const char *text) {
    static uint32_t id = 0x1000;
    mesh_msg_t m = {0};
    m.id = ++id;
    m.from = from;
    if (conv.peer)
        m.to = from == SCENE_ME ? conv.peer : SCENE_ME;
    else
        m.to = MESH_ADDR_BROADCAST;
    m.channel = conv.channel;
    m.rx_time = time;
    m.status = status;
    m.hops = 0xFF;
    snprintf(m.text, sizeof m.text, "%s", text);
    return mesh_msgs_add(conv, &m);
}

// The Ridge Repeater's settings, for the fake remote node to answer with
static void scene_remote(void) {
    mesh_edit_t *r = &fake_remote;
    memset(r, 0, sizeof *r);
    snprintf(r->owner.long_name, sizeof r->owner.long_name, "Ridge Repeater");
    snprintf(r->owner.short_name, sizeof r->owner.short_name, "RDG1");
    r->owner.public_key.size = 32;
    r->device.role = meshtastic_Config_DeviceConfig_Role_ROUTER;
    r->device.node_info_broadcast_secs = 10800;
    r->position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT;
    r->position.fixed_position = true;
    r->position.position_broadcast_secs = 43200;
    r->lora.use_preset = true;
    r->lora.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
    r->lora.region = meshtastic_Config_LoRaConfig_RegionCode_US;
    r->lora.hop_limit = 4;
    r->lora.tx_power = 27;
    r->lora.tx_enabled = true;
    for (int i = 0; i < 8; i++)
        r->channels[i].index = i;
    r->channels[0].role = meshtastic_Channel_Role_PRIMARY;
    r->channels[0].has_settings = true;
    r->channels[1].role = meshtastic_Channel_Role_SECONDARY;
    r->channels[1].has_settings = true;
    snprintf(r->channels[1].settings.name, sizeof r->channels[1].settings.name, "calcmesh");
    fake_remote_error = 0;
}

// --- telemetry ---------------------------------------------------------------

static void tel_device(uint32_t num, uint32_t t, int batt, float volts, float chutil, float airtx,
                       uint32_t uptime) {
    meshtastic_Telemetry m = meshtastic_Telemetry_init_zero;
    m.which_variant = meshtastic_Telemetry_device_metrics_tag;
    meshtastic_DeviceMetrics *d = &m.variant.device_metrics;
    d->has_battery_level = d->has_voltage = d->has_channel_utilization = true;
    d->has_air_util_tx = d->has_uptime_seconds = true;
    d->battery_level = (uint32_t)batt;
    d->voltage = volts;
    d->channel_utilization = chutil;
    d->air_util_tx = airtx;
    d->uptime_seconds = uptime;
    mesh_metrics_add(num, &m, t);
}

static void tel_temp(uint32_t num, uint32_t t, float temp, float rh, float hpa) {
    meshtastic_Telemetry m = meshtastic_Telemetry_init_zero;
    m.which_variant = meshtastic_Telemetry_environment_metrics_tag;
    meshtastic_EnvironmentMetrics *v = &m.variant.environment_metrics;
    v->has_temperature = v->has_relative_humidity = v->has_barometric_pressure = true;
    v->temperature = temp;
    v->relative_humidity = rh;
    v->barometric_pressure = hpa;
    mesh_metrics_add(num, &m, t);
}

static void tel_local(uint32_t num, uint32_t t, uint32_t uptime, uint32_t tx, uint32_t rx) {
    meshtastic_Telemetry m = meshtastic_Telemetry_init_zero;
    m.which_variant = meshtastic_Telemetry_local_stats_tag;
    meshtastic_LocalStats *l = &m.variant.local_stats;
    l->uptime_seconds = uptime;
    l->channel_utilization = 13.4f;
    l->air_util_tx = 1.07f;
    l->num_packets_tx = tx;
    l->num_packets_rx = rx;
    l->num_packets_rx_bad = rx / 140;
    l->num_rx_dupe = rx / 3;
    l->num_tx_relay = tx * 2 / 3;
    l->num_tx_relay_canceled = tx / 5;
    l->num_tx_dropped = 2;
    l->num_online_nodes = 18;
    l->num_total_nodes = 27;
    l->noise_floor = -108;
    l->heap_total_bytes = 296 * 1024;
    l->heap_free_bytes = 118 * 1024;
    mesh_metrics_add(num, &m, t);
}

// A day or so of telemetry from the nodes that send it
static void scene_metrics(void) {
    mesh_metrics_clear();
    // Ours: every minute to the client, kept every ten; the PicoCalc's 18650
    // running down slowly, the channel busier in the evening
    for (int i = 0; i < 8 * 60; i++) {
        uint32_t t = SCENE_NOW - (uint32_t)(8 * 60 - i) * 60;
        float f = i / 480.0f;
        float chu = 9 + 6 * sinf(f * 7) + 3 * sinf(f * 23) + 4 * f;
        tel_device(SCENE_ME, t, 92 - (int)(14 * f), 4.11f - 0.15f * f, chu, 0.9f + 0.4f * f,
                   98000 + (uint32_t)i * 60);
    }
    tel_local(SCENE_ME, SCENE_NOW - 400, 126400, 412, 5310);

    // Ridge Repeater: solar, every 30 min for a day; the cell charges by day
    uint32_t rdg = scene_node(0);
    for (int i = 0; i < 48; i++) {
        uint32_t t = SCENE_NOW - (uint32_t)(48 - i) * 1800 + 40;
        float sun = sinf((i - 38) / 48.0f * 6.2832f); // > 0 by day
        float v = 12.55f + (sun > 0 ? 1.15f * sun : 0.12f * sun) - 0.002f * i;
        tel_device(rdg, t, 80 + (int)(12 * (sun > 0 ? sun : 0)), v, 11 + 3 * sinf(i * 0.9f) + 2 * sinf(i * 0.37f) + (i % 5 == 2 ? 3 : 0),
                   2.4f, 1900000 + (uint32_t)i * 1800);
        tel_temp(rdg, t + 5, 4.5f + 7 * sun, 70 - 15 * sun, 1013.2f - i * 0.05f);
    }
    meshtastic_Telemetry m = meshtastic_Telemetry_init_zero;
    m.which_variant = meshtastic_Telemetry_power_metrics_tag;
    meshtastic_PowerMetrics *w = &m.variant.power_metrics;
    w->has_ch1_voltage = w->has_ch1_current = w->has_ch2_voltage = w->has_ch2_current = true;
    w->has_ch3_voltage = w->has_ch3_current = true;
    w->ch1_voltage = 18.42f, w->ch1_current = 312.5f;
    w->ch2_voltage = 13.21f, w->ch2_current = 201.0f;
    w->ch3_voltage = 5.02f, w->ch3_current = 88.4f;
    mesh_metrics_add(rdg, &m, SCENE_NOW - 1700);

    // Hilltop Solar: panel iced over, the cell going down all day
    uint32_t hts = scene_node(3);
    for (int i = 0; i < 40; i++) {
        uint32_t t = SCENE_NOW - (uint32_t)(40 - i) * 1800 + 600;
        tel_device(hts, t, 71 - (int)(29 * i / 39.0f), 3.96f - 0.34f * i / 39.0f,
                   6 + 2 * sinf(i * 1.3f), 0.6f, 3400000 + (uint32_t)i * 1800);
        tel_temp(hts, t + 3, -1.5f - 2.5f * sinf(i / 6.0f), 93, 871.4f);
    }

    // Weather Stn: everything a weather station has
    uint32_t wx = scene_node(21);
    tel_device(wx, SCENE_NOW - 3000, 101, 5.08f, 12.5f, 0.3f, 640000);
    m = (meshtastic_Telemetry)meshtastic_Telemetry_init_zero;
    m.which_variant = meshtastic_Telemetry_environment_metrics_tag;
    meshtastic_EnvironmentMetrics *v = &m.variant.environment_metrics;
    v->has_temperature = v->has_relative_humidity = v->has_barometric_pressure = true;
    v->temperature = 11.4f, v->relative_humidity = 82, v->barometric_pressure = 1009.6f;
    v->has_wind_speed = v->has_wind_direction = v->has_wind_gust = true;
    v->wind_speed = 3.4f, v->wind_direction = 250, v->wind_gust = 6.1f;
    v->has_rainfall_1h = v->has_rainfall_24h = v->has_lux = true;
    v->rainfall_1h = 0.4f, v->rainfall_24h = 6.2f, v->lux = 1430;
    mesh_metrics_add(wx, &m, SCENE_NOW - 1200);

    // A few that only send device metrics, now and then
    static const struct { int node, batt; float v, chu, air; } dev[] = {
        {1, 87, 4.02f, 10.8f, 0.7f}, {2, 64, 3.84f, 9.2f, 1.9f}, {5, 101, 4.98f, 14.1f, 3.1f},
        {8, 77, 3.93f, 7.5f, 0.2f},  {23, 101, 5.11f, 13.0f, 0.4f}, {4, 15, 3.52f, 4.0f, 0.1f},
    };
    for (size_t i = 0; i < sizeof dev / sizeof dev[0]; i++) {
        uint32_t num = scene_node(dev[i].node);
        const mesh_node_t *n = mesh_db_find(num);
        uint32_t last = n ? n->last_heard : SCENE_NOW;
        for (int k = 3; k >= 0; k--)
            tel_device(num, last - (uint32_t)k * 1800, dev[i].batt + k, dev[i].v + 0.01f * k,
                       dev[i].chu - k, dev[i].air, 500000);
    }
}

// The fake mesh's answer to the request waiting in fake_link.c
bool scene_telemetry_answer(int error) {
    if (!fake_tel_id)
        return false;
    uint32_t id = fake_tel_id, to = fake_tel_to;
    fake_tel_id = 0;
    if (error) {
        mesh_metrics_routing(id, to, (uint8_t)error);
        return true;
    }
    switch (fake_tel_variant) {
    case meshtastic_Telemetry_local_stats_tag:
        tel_local(to, SCENE_NOW, 1987200, 2210, 30412);
        break;
    case meshtastic_Telemetry_device_metrics_tag:
        tel_device(to, SCENE_NOW, 88, 13.05f, 12.1f, 2.5f, 1987200);
        break;
    default:
        mesh_metrics_routing(id, to, meshtastic_Routing_Error_NO_RESPONSE);
        return true;
    }
    mesh_metrics_answer(id, to);
    return true;
}

void scene_load(void) {
    mesh_db_clear();
    mesh_msgs_clear();
    fake_link_state = MESH_LINK_READY;
    ui_set_now(SCENE_NOW);

    meshtastic_MyNodeInfo mi = meshtastic_MyNodeInfo_init_zero;
    mi.my_node_num = SCENE_ME;
    mesh_db_set_my_info(&mi);
    meshtastic_DeviceMetadata md = meshtastic_DeviceMetadata_init_zero;
    snprintf(md.firmware_version, sizeof md.firmware_version, "2.7.11.ee68575");
    mesh_db_set_metadata(&md);

    // The node's settings, as a Heltec v4 would send them
    meshtastic_Config cfg = meshtastic_Config_init_zero;
    cfg.which_payload_variant = meshtastic_Config_device_tag;
    cfg.payload_variant.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    mesh_db_set_config(&cfg);
    cfg = (meshtastic_Config)meshtastic_Config_init_zero;
    cfg.which_payload_variant = meshtastic_Config_position_tag;
    cfg.payload_variant.position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT;
    cfg.payload_variant.position.position_broadcast_secs = 900;
    cfg.payload_variant.position.position_broadcast_smart_enabled = true;
    mesh_db_set_config(&cfg);
    cfg = (meshtastic_Config)meshtastic_Config_init_zero;
    cfg.which_payload_variant = meshtastic_Config_power_tag;
    mesh_db_set_config(&cfg);
    cfg = (meshtastic_Config)meshtastic_Config_init_zero;
    cfg.which_payload_variant = meshtastic_Config_lora_tag;
    cfg.payload_variant.lora.use_preset = true;
    cfg.payload_variant.lora.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
    cfg.payload_variant.lora.region = meshtastic_Config_LoRaConfig_RegionCode_US;
    cfg.payload_variant.lora.hop_limit = 3;
    cfg.payload_variant.lora.tx_enabled = true;
    mesh_db_set_config(&cfg);
    meshtastic_ModuleConfig mc = meshtastic_ModuleConfig_init_zero;
    mc.which_payload_variant = meshtastic_ModuleConfig_mqtt_tag;
    mesh_db_set_module_config(&mc);
    mc.which_payload_variant = meshtastic_ModuleConfig_range_test_tag;
    mc.payload_variant.range_test.enabled = true;
    mesh_db_set_module_config(&mc);
    mc = (meshtastic_ModuleConfig)meshtastic_ModuleConfig_init_zero;
    mc.which_payload_variant = meshtastic_ModuleConfig_neighbor_info_tag;
    mc.payload_variant.neighbor_info.enabled = true;
    mesh_db_set_module_config(&mc);
    cfg = (meshtastic_Config)meshtastic_Config_init_zero;
    cfg.which_payload_variant = meshtastic_Config_bluetooth_tag;
    cfg.payload_variant.bluetooth.enabled = true;
    mesh_db_set_config(&cfg);

    add_channel(0, meshtastic_Channel_Role_PRIMARY, "");
    add_channel(1, meshtastic_Channel_Role_SECONDARY, "calcmesh");
    add_channel(2, meshtastic_Channel_Role_SECONDARY, "hikers");

    add_node(SCENE_ME, "PicoCalc", "PCAL", 0, 0, 0, 101, false);
    mesh_edit_select(0);
    mesh_edit_load();
    scene_remote();
    settings_clear();
    settings_set_int(SET_SCREEN_BACKLIGHT, 128);
    settings_set_int(SET_KEYS_BACKLIGHT, 64);
    for (int i = 0; i < NNODES; i++)
        add_node(scene_node(i), nodes[i].lname, nodes[i].sname, nodes[i].hops,
                 nodes[i].snr, nodes[i].ago, nodes[i].batt, nodes[i].fav);
    scene_metrics();
    place(SCENE_ME, 0, 0, 55);
    for (size_t i = 0; i < sizeof places / sizeof places[0]; i++)
        place(scene_node(places[i].node), places[i].north, places[i].east,
              places[i].alt);

    // #LongFast
    uint32_t t = SCENE_NOW - 7200;
    struct { int who; const char *text; } lf[] = {
        {1, "Morning all. Anyone hear the Ridge repeater this side of the river?"},
        {0, "Loud and clear from the north end, SNR around 9."},
        {-1, "Hearing it here too, through the window even."},
        {4, "Coming through on 2 hops from the trailhead lot."},
        {2, "Has anyone tried the new firmware on a T-Beam? Mine keeps rebooting "
            "when GPS gets a fix, wondering if it's the power settings or just me."},
        {6, "Reverting fixed it for me. Haven't dug into why."},
        {-1, "Same build on a Heltec v4 here and it's been solid for a week."},
        {8, "Test from the boat, ignore"},
        {1, "Got it, 4 hops."},
        {17, "¿Alguien en el café? ñandú test, UTF-8 ✓"},
        {3, "Hilltop panel's iced over again, battery dropping. Will swap tomorrow."},
    };
    for (size_t i = 0; i < sizeof lf / sizeof lf[0]; i++, t += 600) {
        uint32_t from = lf[i].who < 0 ? SCENE_ME : scene_node(lf[i].who);
        scene_msg(from, MESH_CONV_CHANNEL(0), t,
                  from == SCENE_ME ? MESH_MSG_RELAYED : MESH_MSG_RECEIVED,
                  lf[i].text);
        if (i == 7) // read up to here
            mesh_msgs_mark_read(MESH_CONV_CHANNEL(0));
    }

    // #calcmesh
    scene_msg(scene_node(23), MESH_CONV_CHANNEL(1), SCENE_NOW - 900,
              MESH_MSG_RECEIVED, "Keyboard driver works, framebuffer next.");
    scene_msg(SCENE_ME, MESH_CONV_CHANNEL(1), SCENE_NOW - 600, MESH_MSG_SENT,
              "Typing this on the PicoCalc!");

    // DMs
    mesh_conv_t juni = MESH_CONV_DM(scene_node(1));
    scene_msg(scene_node(1), juni, SCENE_NOW - 3000, MESH_MSG_RECEIVED,
              "Are you bringing the spare antenna Saturday?");
    scene_msg(SCENE_ME, juni, SCENE_NOW - 2900, MESH_MSG_DELIVERED,
              "Yes, the 5/8 wave and some SMA adapters.");
    scene_msg(scene_node(1), juni, SCENE_NOW - 2800, MESH_MSG_RECEIVED,
              "Perfect, thanks.");
    scene_msg(SCENE_ME, juni, SCENE_NOW - 60, MESH_MSG_FAILED,
              "Running 10 min late");
    scene_msg(SCENE_ME, juni, SCENE_NOW - 30, MESH_MSG_QUEUED,
              "Running 10 min late (again)");
    mesh_msgs_mark_read(juni);
    scene_msg(scene_node(0), MESH_CONV_DM(scene_node(0)), SCENE_NOW - 120,
              MESH_MSG_RECEIVED, "Repeater battery voltage alert cleared.");
}
