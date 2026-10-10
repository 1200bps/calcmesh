#include "scene.h"

#include <stdio.h>
#include <string.h>

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
    {"Hilltop Solar", "HTS", 0, -11.75f, 610, 42, false},
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
    mesh_db_upsert_node(&ni);
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
    mesh_edit_load();
    settings_clear();
    settings_set_int(SET_SCREEN_BACKLIGHT, 128);
    settings_set_int(SET_KEYS_BACKLIGHT, 64);
    for (int i = 0; i < NNODES; i++)
        add_node(scene_node(i), nodes[i].lname, nodes[i].sname, nodes[i].hops,
                 nodes[i].snr, nodes[i].ago, nodes[i].batt, nodes[i].fav);
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
