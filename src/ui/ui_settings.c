// Settings tab: the node's settings, then calcmesh's own, in one list.
//
// Node settings are edited on a working copy (mesh/nodeedit.h): a changed
// field gets a * and shows what it was, and nothing reaches the node until
// "Write changes to node", which asks first. calcmesh's settings
// (sd/settings.h) take effect at once and are saved to the SD card.
//
// Up/Down pick a field, Left/Right step its value, Enter edits text (or
// toggles a switch, or runs an action).

#include <stdio.h>
#include <string.h>

#include "link.h"
#include "mesh/nodeedit.h"
#include "sd/sd.h"
#include "sd/settings.h"
#include "ui.h"
#include "ui_internal.h"

#define C_LABEL 2
#define W_LABEL 16
#define C_VALUE 19
#define W_VALUE (UI_COLS - C_VALUE - 1)

// One line per row, minus the hint line at the bottom
#define ROWS (UI_BODY_H / UI_LH - 1)

typedef enum {
    // Node: owner
    F_LONG_NAME,
    F_SHORT_NAME,
    F_ROLE,
    F_NODEINFO_SECS,
    // LoRa
    F_REGION,
    F_PRESET,
    F_HOPS,
    F_TX_POWER,
    F_TX_ENABLED,
    // Channels (arg: index)
    F_CHANNEL,
    // Position, power, Bluetooth
    F_GPS,
    F_POS_SECS,
    F_POS_SMART,
    F_FIXED,
    F_POWER_SAVE,
    F_BLUETOOTH,
    F_NODE_LAST = F_BLUETOOTH,
    // calcmesh
    F_LCD,
    F_KBD,
    F_DESKTOP,
    F_IDLE,
    F_ECHO,
    // Actions
    A_WRITE,
    A_DISCARD,
} field_t;

typedef enum { K_TEXT, K_ENUM, K_BOOL, K_INT, K_ACTION } kind_t;

typedef enum {
    R_HEAD,  // section heading
    R_NOTE,  // a line of text
    R_FIELD, // selectable
} row_kind_t;

typedef struct {
    uint8_t kind; // row_kind_t
    uint8_t field;
    uint8_t arg;
    const char *text; // R_HEAD and R_NOTE
} row_t;

#define MAX_ROWS 48
static row_t rows[MAX_ROWS];
static int nrows;
static int sel = -1, top; // sel: index into rows
static int sel_field = -1, sel_arg; // keeps the selection across rebuilds

// Text being edited
static bool editing;
static char edit_buf[48];
static int edit_max, edit_cursor;
// The write confirmation
static bool confirming;
static const char *notice;

// --- names -------------------------------------------------------------------

static const char *const region_names[] = {
    "Unset", "US", "EU_433", "EU_868", "CN", "JP", "ANZ", "KR", "TW", "RU",
    "IN", "NZ_865", "TH", "LORA_24", "UA_433", "UA_868", "MY_433", "MY_919",
    "SG_923", "PH_433", "PH_868", "PH_915", "ANZ_433", "KZ_433", "KZ_863",
    "NP_865", "BR_902", "ITU1_2M", "ITU2_2M", "EU_866", "EU_874", "EU_917",
    "EU_N_868", "ITU3_2M", "ITU1_70CM", "ITU2_70CM", "ITU3_70CM", "ITU2_125CM",
};
#define N_REGIONS ((int)(sizeof region_names / sizeof region_names[0]))

static const char *const role_names[] = {
    "Client", "Client mute", "Router", "Router client", "Repeater", "Tracker",
    "Sensor", "TAK", "Client hidden", "Lost and found", "TAK tracker",
    "Router late", "Client base",
};
#define N_ROLES ((int)(sizeof role_names / sizeof role_names[0]))

static const char *const gps_names[] = {"Off", "On", "Not present"};

#define N_PRESETS 17 // mesh_db_preset_name() knows 0..16

// Position broadcast steps, seconds (0: the firmware's default)
static const int pos_steps[] = {0, 60, 120, 300, 600, 900, 1800, 3600,
                                7200, 21600, 43200, 86400};
#define N_POS_STEPS ((int)(sizeof pos_steps / sizeof pos_steps[0]))
// NodeInfo broadcast steps (0: the firmware's default, 3h; it won't go under 1h)
static const int nodeinfo_steps[] = {0, 3600, 7200, 10800, 21600, 43200, 86400};
#define N_NODEINFO_STEPS ((int)(sizeof nodeinfo_steps / sizeof nodeinfo_steps[0]))

// The next step up or down from v, which may sit between steps
static bool next_step(const int *steps, int n_steps, int v, int dir, int *out) {
    int i = dir > 0 ? 0 : n_steps - 1;
    while (i >= 0 && i < n_steps && (dir > 0 ? steps[i] <= v : steps[i] >= v))
        i += dir;
    if (i < 0 || i >= n_steps)
        return false;
    *out = steps[i];
    return true;
}

static const char *name_of(const char *const *names, int n, int v, char *buf) {
    if (v >= 0 && v < n && names[v])
        return names[v];
    snprintf(buf, 12, "#%d", v);
    return buf;
}

static void fmt_secs(int s, char *buf) {
    if (s == 0)
        strcpy(buf, "default");
    else if (s % 3600 == 0)
        snprintf(buf, 12, "%dh", s / 3600);
    else if (s % 60 == 0)
        snprintf(buf, 12, "%dm", s / 60);
    else
        snprintf(buf, 12, "%ds", s);
}

// --- fields ------------------------------------------------------------------

static kind_t kind_of(int f) {
    switch (f) {
    case F_LONG_NAME:
    case F_SHORT_NAME:
    case F_CHANNEL:
        return K_TEXT;
    case F_TX_ENABLED:
    case F_POS_SMART:
    case F_FIXED:
    case F_POWER_SAVE:
    case F_BLUETOOTH:
    case F_ECHO:
        return K_BOOL;
    case F_HOPS:
    case F_TX_POWER:
    case F_LCD:
    case F_KBD:
    case F_IDLE:
        return K_INT;
    case A_WRITE:
    case A_DISCARD:
        return K_ACTION;
    default:
        return K_ENUM;
    }
}

static bool is_node(int f) { return f <= F_NODE_LAST; }

static uint32_t section_of(int f, int arg) {
    switch (f) {
    case F_LONG_NAME:
    case F_SHORT_NAME: return MESH_EDIT_OWNER;
    case F_ROLE:
    case F_NODEINFO_SECS: return MESH_EDIT_DEVICE;
    case F_REGION:
    case F_PRESET:
    case F_HOPS:
    case F_TX_POWER:
    case F_TX_ENABLED: return MESH_EDIT_LORA;
    case F_CHANNEL: return MESH_EDIT_CHANNEL(arg);
    case F_GPS:
    case F_POS_SECS:
    case F_POS_SMART:
    case F_FIXED: return MESH_EDIT_POSITION;
    case F_POWER_SAVE: return MESH_EDIT_POWER;
    case F_BLUETOOTH: return MESH_EDIT_BLUETOOTH;
    default: return 0;
    }
}

static const char *label_of(int f, int arg) {
    static char buf[20];
    switch (f) {
    case F_LONG_NAME: return "Long name";
    case F_SHORT_NAME: return "Short name";
    case F_ROLE: return "Role";
    case F_NODEINFO_SECS: return "Send NodeInfo";
    case F_REGION: return "Region";
    case F_PRESET: return "Modem preset";
    case F_HOPS: return "Hop limit";
    case F_TX_POWER: return "TX power";
    case F_TX_ENABLED: return "Transmit";
    case F_CHANNEL:
        snprintf(buf, sizeof buf, "Channel %d name", arg);
        return buf;
    case F_GPS: return "GPS";
    case F_POS_SECS: return "Send position";
    case F_POS_SMART: return "Smart position";
    case F_FIXED: return "Fixed position";
    case F_POWER_SAVE: return "Power saving";
    case F_BLUETOOTH: return "Bluetooth";
    case F_LCD: return "Screen light";
    case F_KBD: return "Keyboard light";
    case F_DESKTOP: return "Desktop mode";
    case F_IDLE: return "Desktop timeout";
    case F_ECHO: return "Key echo (USB)";
    default: return "";
    }
}

// Text fields: the string in e, and its size
static char *text_of(mesh_edit_t *e, int f, int arg, int *size) {
    switch (f) {
    case F_LONG_NAME:
        *size = sizeof e->owner.long_name;
        return e->owner.long_name;
    case F_SHORT_NAME:
        *size = sizeof e->owner.short_name;
        return e->owner.short_name;
    case F_CHANNEL:
        *size = sizeof e->channels[arg].settings.name;
        return e->channels[arg].settings.name;
    default:
        *size = 0;
        return NULL;
    }
}

// calcmesh's settings: what the keyboard keeps (screen: multiples of 16,
// 16-240; keys: multiples of 32, 0 off) and what desktop mode allows
#define LCD_MIN 16
#define LCD_MAX 240
#define LCD_STEP 16
#define KBD_MAX 224
#define KBD_STEP 32
#define IDLE_MIN 6 // clients heartbeat every 5 minutes
#define IDLE_MAX 120
#define IDLE_DEFAULT 10
// Minutes: fine steps near the default, coarse ones further out
static const int idle_steps[] = {6, 7, 8, 9, 10, 12, 15, 20, 30, 45, 60, 90, 120};
#define N_IDLE_STEPS ((int)(sizeof idle_steps / sizeof idle_steps[0]))

static const char *const desktop_names[] = {"auto", "on", "off"}; // meshlink_desktop_t

static int desktop_mode(void) {
    const char *v = settings_get(SET_DESKTOP_MODE);
    for (int i = 0; v && i < 3; i++)
        if (strcmp(v, desktop_names[i]) == 0)
            return i;
    return 0;
}

// Number fields (ints, enums, switches): from the edit copy for the node's,
// from the settings table for ours
static int get(const mesh_edit_t *e, int f) {
    switch (f) {
    case F_ROLE: return e->device.role;
    case F_NODEINFO_SECS: return (int)e->device.node_info_broadcast_secs;
    case F_REGION: return e->lora.region;
    case F_PRESET: return e->lora.modem_preset;
    case F_HOPS: return (int)e->lora.hop_limit;
    case F_TX_POWER: return e->lora.tx_power;
    case F_TX_ENABLED: return e->lora.tx_enabled;
    case F_GPS: return e->position.gps_mode;
    case F_POS_SECS: return (int)e->position.position_broadcast_secs;
    case F_POS_SMART: return e->position.position_broadcast_smart_enabled;
    case F_FIXED: return e->position.fixed_position;
    case F_POWER_SAVE: return e->power.is_power_saving;
    case F_BLUETOOTH: return e->bluetooth.enabled;
    case F_LCD: return (int)settings_get_int(SET_SCREEN_BACKLIGHT, -1);
    case F_KBD: return (int)settings_get_int(SET_KEYS_BACKLIGHT, -1);
    case F_DESKTOP: return desktop_mode();
    case F_IDLE: return (int)settings_get_int(SET_DESKTOP_IDLE, IDLE_DEFAULT);
    case F_ECHO: return settings_get_bool(SET_KEY_ECHO, false);
    default: return 0;
    }
}

static void set(mesh_edit_t *e, int f, int v) {
    switch (f) {
    case F_ROLE: e->device.role = (meshtastic_Config_DeviceConfig_Role)v; break;
    case F_NODEINFO_SECS: e->device.node_info_broadcast_secs = (uint32_t)v; break;
    case F_REGION: e->lora.region = (meshtastic_Config_LoRaConfig_RegionCode)v; break;
    case F_PRESET: e->lora.modem_preset = (meshtastic_Config_LoRaConfig_ModemPreset)v; break;
    case F_HOPS: e->lora.hop_limit = (uint32_t)v; break;
    case F_TX_POWER: e->lora.tx_power = v; break;
    case F_TX_ENABLED: e->lora.tx_enabled = v; break;
    case F_GPS: e->position.gps_mode = (meshtastic_Config_PositionConfig_GpsMode)v; break;
    case F_POS_SECS: e->position.position_broadcast_secs = (uint32_t)v; break;
    case F_POS_SMART: e->position.position_broadcast_smart_enabled = v; break;
    case F_FIXED: e->position.fixed_position = v; break;
    case F_POWER_SAVE: e->power.is_power_saving = v; break;
    case F_BLUETOOTH: e->bluetooth.enabled = v; break;
    // Ours take effect from the table (backlight.c, main.c) and get saved
    case F_LCD: settings_set_int(SET_SCREEN_BACKLIGHT, v); return;
    case F_KBD: settings_set_int(SET_KEYS_BACKLIGHT, v); return;
    case F_DESKTOP: settings_set(SET_DESKTOP_MODE, desktop_names[v]); return;
    case F_IDLE: settings_set_int(SET_DESKTOP_IDLE, v); return;
    case F_ECHO: settings_set_bool(SET_KEY_ECHO, v); return;
    default: return;
    }
    mesh_edit_ack(); // the last write's result has been seen
}

// The value as shown
static const char *value_str(const mesh_edit_t *e, int f, int arg, char *buf) {
    int v = get(e, f);
    switch (kind_of(f)) {
    case K_TEXT: {
        int size;
        const char *t = text_of((mesh_edit_t *)e, f, arg, &size);
        if (f == F_CHANNEL && !*t) {
            // An unnamed channel goes by its preset's name
            snprintf(buf, 24, "(%s)",
                     e->lora.use_preset ? mesh_db_preset_name((uint8_t)e->lora.modem_preset)
                                        : "Custom");
            return buf;
        }
        return t;
    }
    case K_BOOL: return v ? "On" : "Off";
    default: break;
    }
    switch (f) {
    case F_ROLE: return name_of(role_names, N_ROLES, v, buf);
    case F_REGION: return name_of(region_names, N_REGIONS, v, buf);
    case F_PRESET:
        return e->lora.use_preset ? mesh_db_preset_name((uint8_t)v) : "Custom (not a preset)";
    case F_HOPS: snprintf(buf, 12, "%d", v); return buf;
    case F_TX_POWER:
        if (v)
            snprintf(buf, 12, "%d dBm", v);
        else
            strcpy(buf, "default");
        return buf;
    case F_GPS: return name_of(gps_names, 3, v, buf);
    case F_POS_SECS: fmt_secs(v, buf); return buf;
    case F_NODEINFO_SECS:
        if (v == 0)
            return "default (3h)";
        fmt_secs(v, buf);
        return buf;
    case F_LCD:
    case F_KBD:
        if (v < 0)
            strcpy(buf, "?");
        else if (v == 0 || (f == F_KBD && (v < KBD_STEP || v > 240)))
            strcpy(buf, "Off");
        else
            snprintf(buf, 12, "%d", v);
        return buf;
    case F_DESKTOP: return desktop_names[v];
    case F_IDLE: snprintf(buf, 12, "%d min", v); return buf;
    default: return "";
    }
}

// Node field differs from what the node has
static bool changed(int f, int arg) {
    if (!is_node(f))
        return false;
    if (kind_of(f) == K_TEXT) {
        int size;
        return strcmp(text_of(&mesh_edit, f, arg, &size),
                      text_of(&mesh_edit_base, f, arg, &size)) != 0;
    }
    return get(&mesh_edit, f) != get(&mesh_edit_base, f);
}

static bool node_editable(void) {
    return mesh_link_conn() == MESH_CONN_UP && mesh_edit_state() != MESH_EDIT_RESTARTING;
}

static bool available(int f, int arg) {
    if (!is_node(f))
        return true;
    return (mesh_edit_available() & section_of(f, arg)) != 0;
}

// Step a value one notch in dir (+1/-1). False if it can't move.
static bool step(int f, int dir) {
    mesh_edit_t *e = &mesh_edit;
    int v = get(e, f), n = v;
    switch (f) {
    case F_ROLE:
        do
            n = (n + dir + N_ROLES) % N_ROLES;
        while (n == 3 || n == 4); // Router client and Repeater: deprecated
        break;
    case F_REGION: // never back to Unset
        n = (n - 1 + dir + N_REGIONS - 1) % (N_REGIONS - 1) + 1;
        break;
    case F_PRESET:
        if (!e->lora.use_preset)
            return false;
        n = (n + dir + N_PRESETS) % N_PRESETS;
        break;
    case F_HOPS: n = v + dir; if (n < 1 || n > 7) return false; break;
    case F_TX_POWER: n = v + dir; if (n < 0 || n > 30) return false; break;
    case F_GPS: n = (v + dir + 3) % 3; break;
    case F_POS_SECS:
        if (!next_step(pos_steps, N_POS_STEPS, v, dir, &n))
            return false;
        break;
    case F_NODEINFO_SECS:
        if (!next_step(nodeinfo_steps, N_NODEINFO_STEPS, v, dir, &n))
            return false;
        break;
    case F_LCD:
        // Onto a step first, if it's between (the keyboard's own keys)
        n = v < 0 ? 128 : v - v % LCD_STEP + (dir > 0 || v % LCD_STEP == 0 ? dir * LCD_STEP : 0);
        if (n < LCD_MIN || n > LCD_MAX)
            return false;
        break;
    case F_KBD:
        if (v < KBD_STEP || v > 240) // the keyboard's off
            v = 0;
        n = v - v % KBD_STEP + (dir > 0 || v % KBD_STEP == 0 ? dir * KBD_STEP : 0);
        if (n < 0 || n > KBD_MAX)
            return false;
        break;
    case F_DESKTOP: n = (v + dir + 3) % 3; break;
    case F_IDLE:
        if (!next_step(idle_steps, N_IDLE_STEPS, v, dir, &n))
            return false;
        break;
    default:
        if (kind_of(f) == K_BOOL)
            n = !v;
        else
            return false;
    }
    set(e, f, n);
    return true;
}

// --- the list ----------------------------------------------------------------

static void add(row_kind_t k, int f, int arg, const char *text) {
    if (nrows < MAX_ROWS)
        rows[nrows++] = (row_t){(uint8_t)k, (uint8_t)f, (uint8_t)arg, text};
}

static void add_field(int f, int arg) {
    if (available(f, arg))
        add(R_FIELD, f, arg, NULL);
}

static int changes(void) {
    int n = 0;
    for (int i = 0; i < nrows; i++)
        if (rows[i].kind == R_FIELD && changed(rows[i].field, rows[i].arg))
            n++;
    return n;
}

static const char *edit_result(void) {
    static char buf[64];
    switch (mesh_edit_state()) {
    case MESH_EDIT_RESTARTING: return "Written. Waiting for the node to restart...";
    case MESH_EDIT_DONE: return "The node restarted and confirms the changes.";
    case MESH_EDIT_MISMATCH: {
        static const char *const names[] = {"names", "role", "position", "power",
                                            "LoRa", "Bluetooth"};
        uint32_t m = mesh_edit_mismatch();
        int n = snprintf(buf, sizeof buf, "Restarted, but these didn't take:");
        for (int i = 0; i < 6; i++)
            if (m & (1u << i) && n < (int)sizeof buf)
                n += snprintf(buf + n, sizeof buf - n, " %s", names[i]);
        if (m & MESH_EDIT_CHANNELS && n < (int)sizeof buf)
            snprintf(buf + n, sizeof buf - n, " channels");
        return buf;
    }
    case MESH_EDIT_NO_RESTART: return "The node hasn't come back. Check on it.";
    case MESH_EDIT_REFUSED:
        snprintf(buf, sizeof buf, "The node refused the change (error %u).",
                 mesh_link_stats()->admin_last_error);
        return buf;
    default: return NULL;
    }
}

// "Modules on: MQTT, Range test", from the module config the node sent
static const char *modules_on(void) {
    static char buf[80];
    static const struct {
        pb_size_t tag;
        const char *name;
    } mods[] = {
        {meshtastic_ModuleConfig_mqtt_tag, "MQTT"},
        {meshtastic_ModuleConfig_serial_tag, "Serial"},
        {meshtastic_ModuleConfig_external_notification_tag, "Ext notify"},
        {meshtastic_ModuleConfig_store_forward_tag, "S&F"},
        {meshtastic_ModuleConfig_range_test_tag, "Range test"},
        {meshtastic_ModuleConfig_neighbor_info_tag, "Neighbors"},
        {meshtastic_ModuleConfig_detection_sensor_tag, "Detection"},
        {meshtastic_ModuleConfig_paxcounter_tag, "Paxcounter"},
        {meshtastic_ModuleConfig_canned_message_tag, "Canned msgs"},
    };
    if (!mesh_db.module_have)
        return NULL;
    int n = snprintf(buf, sizeof buf, "Modules on:");
    bool any = false;
    for (size_t i = 0; i < sizeof mods / sizeof mods[0]; i++) {
        const meshtastic_ModuleConfig *m = mesh_db_module(mods[i].tag);
        if (!m)
            continue;
        bool on;
        switch (mods[i].tag) {
        case meshtastic_ModuleConfig_mqtt_tag: on = m->payload_variant.mqtt.enabled; break;
        case meshtastic_ModuleConfig_serial_tag: on = m->payload_variant.serial.enabled; break;
        case meshtastic_ModuleConfig_external_notification_tag:
            on = m->payload_variant.external_notification.enabled;
            break;
        case meshtastic_ModuleConfig_store_forward_tag:
            on = m->payload_variant.store_forward.enabled;
            break;
        case meshtastic_ModuleConfig_range_test_tag: on = m->payload_variant.range_test.enabled; break;
        case meshtastic_ModuleConfig_neighbor_info_tag:
            on = m->payload_variant.neighbor_info.enabled;
            break;
        case meshtastic_ModuleConfig_detection_sensor_tag:
            on = m->payload_variant.detection_sensor.enabled;
            break;
        case meshtastic_ModuleConfig_paxcounter_tag: on = m->payload_variant.paxcounter.enabled; break;
        default: on = m->payload_variant.canned_message.enabled; break;
        }
        if (on && n < (int)sizeof buf) {
            n += snprintf(buf + n, sizeof buf - n, "%s %s", any ? "," : "", mods[i].name);
            any = true;
        }
    }
    if (!any)
        snprintf(buf + n, sizeof buf - n, " none");
    return buf;
}

static void build(void) {
    nrows = 0;
    static char head[48];
    if (mesh_db.my_node_num)
        snprintf(head, sizeof head, "Node %s  !%08lx  fw %s", ui_node_short(mesh_db.my_node_num),
                 (unsigned long)mesh_db.my_node_num,
                 mesh_db.firmware_version[0] ? mesh_db.firmware_version : "?");
    else
        snprintf(head, sizeof head, "Node");
    add(R_HEAD, 0, 0, head);

    mesh_link_conn_t c = mesh_link_conn();
    if (!mesh_edit_available()) {
        add(R_NOTE, 0, 0,
            c == MESH_CONN_DESKTOP ? "Desktop mode has the node. Its settings show once"
                                   : "The node's settings show here once it's connected.");
        if (c == MESH_CONN_DESKTOP)
            add(R_NOTE, 0, 0, "it hands the node back.");
    } else {
        const char *r = edit_result();
        if (r)
            add(R_NOTE, 0, 0, r);
        else if (c == MESH_CONN_DESKTOP)
            add(R_NOTE, 0, 0, "Desktop mode has the node: look, don't touch.");
        else if (c != MESH_CONN_UP)
            add(R_NOTE, 0, 0, "Not connected to the node: read only.");
        add_field(F_LONG_NAME, 0);
        add_field(F_SHORT_NAME, 0);
        add_field(F_ROLE, 0);
        add_field(F_NODEINFO_SECS, 0);
        add_field(F_REGION, 0);
        add_field(F_PRESET, 0);
        add_field(F_HOPS, 0);
        add_field(F_TX_POWER, 0);
        add_field(F_TX_ENABLED, 0);
        for (int i = 0; i < MESH_MAX_CHANNELS; i++)
            if (mesh_edit_base.channels[i].role != meshtastic_Channel_Role_DISABLED)
                add_field(F_CHANNEL, i);
        add_field(F_GPS, 0);
        add_field(F_POS_SECS, 0);
        add_field(F_POS_SMART, 0);
        add_field(F_FIXED, 0);
        add_field(F_POWER_SAVE, 0);
        add_field(F_BLUETOOTH, 0);
        const char *mods = modules_on();
        if (mods)
            add(R_NOTE, 0, 0, mods);
        if (mesh_edit_dirty() && mesh_edit_state() != MESH_EDIT_RESTARTING) {
            add(R_FIELD, A_WRITE, 0, NULL);
            add(R_FIELD, A_DISCARD, 0, NULL);
        }
    }

    add(R_HEAD, 0, 0, "calcmesh");
    add_field(F_LCD, 0);
    add_field(F_KBD, 0);
    add_field(F_DESKTOP, 0);
    add_field(F_IDLE, 0);
    add_field(F_ECHO, 0);
    if (sd_state() == SD_READY)
        add(R_NOTE, 0, 0, settings_dirty() ? "Saving to the SD card..." : "Saved on the SD card.");
    else
        add(R_NOTE, 0, 0, "No SD card: these last until power off.");

    // Put the selection back on the same field, or the first one
    sel = -1;
    for (int i = 0; i < nrows; i++)
        if (rows[i].kind == R_FIELD && rows[i].field == sel_field && rows[i].arg == sel_arg)
            sel = i;
    if (sel < 0)
        for (int i = 0; i < nrows && sel < 0; i++)
            if (rows[i].kind == R_FIELD)
                sel = i;
    if (sel >= 0) {
        sel_field = rows[sel].field;
        sel_arg = rows[sel].arg;
    }
}

static void move(int dir) {
    for (int i = sel + dir; i >= 0 && i < nrows; i += dir)
        if (rows[i].kind == R_FIELD) {
            sel = i;
            sel_field = rows[i].field;
            sel_arg = rows[i].arg;
            return;
        }
    // Nothing more to select that way: still scroll to the end
    if (dir < 0)
        top = 0;
}

bool ui_settings_pending(void) {
    return mesh_edit_state() != MESH_EDIT_RESTARTING && mesh_edit_dirty() != 0;
}
bool ui_settings_modal(void) { return editing || confirming; }

// --- drawing -------------------------------------------------------------------

static void draw_row(const row_t *r, int y, bool selected) {
    char buf[64];
    if (r->kind == R_HEAD) {
        ui_cells(0, y, UI_COLS, r->text, FB_WHITE, FB_CLEAR);
        fb_hline(0, y + UI_LH - 1, FB_W, FB_DIM);
        return;
    }
    if (r->kind == R_NOTE) {
        ui_cells(C_LABEL, y, UI_COLS - C_LABEL, r->text, FB_GREY, FB_CLEAR);
        return;
    }
    int f = r->field;
    uint8_t fg = selected ? FB_BLACK : FB_WHITE;
    uint8_t dim = selected ? FB_BLACK : FB_GREY;
    if (selected)
        fb_fill(0, y, FB_W, UI_LH, FB_WHITE);

    if (f == A_WRITE || f == A_DISCARD) {
        if (f == A_WRITE) {
            int n = changes();
            snprintf(buf, sizeof buf, "> Write %d change%s to the node", n, n == 1 ? "" : "s");
        } else {
            snprintf(buf, sizeof buf, "> Discard changes");
        }
        ui_cells(C_LABEL, y, UI_COLS - C_LABEL, buf, fg, FB_CLEAR);
        return;
    }

    bool ch = changed(f, r->arg);
    bool ro = is_node(f) && !node_editable();
    if (ch)
        ui_cells(0, y, 1, "*", fg, FB_CLEAR);
    ui_cells(C_LABEL, y, W_LABEL, label_of(f, r->arg), ro ? dim : fg, FB_CLEAR);

    if (editing && selected) {
        // The line being edited, with a block cursor
        fb_fill(C_VALUE * UI_CW - 2, y, (W_VALUE + 1) * UI_CW, UI_LH, FB_BLACK);
        ui_cells(C_VALUE, y, W_VALUE, edit_buf, FB_WHITE, FB_CLEAR);
        char under[2] = {edit_buf[edit_cursor] ? edit_buf[edit_cursor] : ' ', 0};
        ui_cells(C_VALUE + edit_cursor, y, 1, under, FB_BLACK, FB_WHITE);
        return;
    }

    const char *v = value_str(&mesh_edit, f, r->arg, buf);
    int x = C_VALUE;
    if (f == F_LCD || f == F_KBD) {
        // A bar, then the number
        int lv = get(&mesh_edit, f), full = f == F_LCD ? LCD_MAX : KBD_MAX;
        if (f == F_KBD && lv > 240)
            lv = 0;
        int bw = 10 * UI_CW, bx = x * UI_CW, by = y + 3;
        fb_rect(bx, by, bw, UI_LH - 6, dim);
        if (lv > 0)
            fb_fill(bx + 1, by + 1, (bw - 2) * (lv > full ? full : lv) / full, UI_LH - 8, fg);
        x += 11;
    }
    ui_cells(x, y, C_VALUE + W_VALUE - x, v, ro ? dim : fg, FB_CLEAR);
    if (ch) {
        // What the node has now
        char was[64], wbuf[24];
        snprintf(was, sizeof was, "(was %s)", value_str(&mesh_edit_base, f, r->arg, wbuf));
        int used = utf8_cells(v) + 1;
        if (x + used < C_VALUE + W_VALUE)
            ui_cells(x + used, y, C_VALUE + W_VALUE - x - used, was, dim, FB_CLEAR);
    }
}

static const char *hint(void) {
    if (notice)
        return notice;
    if (editing)
        return "Enter: done  Esc: cancel";
    if (sel < 0)
        return "";
    int f = rows[sel].field;
    if (is_node(f) && !node_editable())
        return "Read only while the node isn't connected.";
    switch (kind_of(f)) {
    case K_TEXT: return "Enter: edit";
    case K_BOOL: return "Enter or Left/Right: switch";
    case K_ACTION: return f == A_WRITE ? "Enter: review and write" : "Enter: back to the node's values";
    default: return "Left/Right: change";
    }
}

// Word-wrapped lines of up to w cells; advances *y past them. c FB_CLEAR:
// only measure.
static void wrap_text(const char *s, int x, int *y, int w, uint8_t c) {
    char line[64];
    while (*s) {
        int n = (int)strlen(s), len = n;
        if (n > w) {
            len = w;
            while (len > 0 && s[len] != ' ')
                len--;
            if (!len)
                len = w;
        }
        memcpy(line, s, (size_t)len);
        line[len] = '\0';
        if (c != FB_CLEAR)
            ui_cells(x, *y, w, line, c, FB_CLEAR);
        *y += UI_LH;
        s += len;
        while (*s == ' ')
            s++;
    }
}

#define RESTART_TEXT "The node saves these and restarts."
#define RISK_TEXT                                                              \
    "! Region, preset, transmit and channel changes can cut it off from the "  \
    "rest of the mesh until the other nodes match."

// The confirmation: what will change, and what can go wrong
static void draw_confirm(void) {
    int x0 = 2, w = FB_W - 4;
    int y0 = UI_BODY_Y + 2;
    uint32_t d = mesh_edit_dirty();
    bool risky = (d & (MESH_EDIT_LORA | MESH_EDIT_CHANNELS)) != 0;
    // Title, changes, gap, text, gap, keys
    int h = (UI_LH + 5) + changes() * UI_LH + UI_LH / 2;
    wrap_text(RESTART_TEXT, 0, &h, UI_COLS - 4, FB_CLEAR);
    if (risky)
        wrap_text(RISK_TEXT, 0, &h, UI_COLS - 4, FB_CLEAR);
    h += UI_LH / 2 + UI_LH + 3;
    fb_fill(x0, y0, w, h, FB_BLACK);
    fb_rect(x0, y0, w, h, FB_WHITE);
    fb_fill(x0, y0, w, UI_LH + 2, FB_WHITE);

    char buf[96];
    snprintf(buf, sizeof buf, "Write to %s?", ui_node_short(mesh_db.my_node_num));
    ui_cells(1, y0 + 1, UI_COLS - 2, buf, FB_BLACK, FB_CLEAR);

    int y = y0 + UI_LH + 5;
    for (int i = 0; i < nrows; i++) {
        const row_t *r = &rows[i];
        if (r->kind != R_FIELD || !changed(r->field, r->arg))
            continue;
        char a[24], b[24], old[32];
        snprintf(old, sizeof old, "%s", value_str(&mesh_edit_base, r->field, r->arg, a));
        snprintf(buf, sizeof buf, "%s -> %s", old, value_str(&mesh_edit, r->field, r->arg, b));
        ui_cells(2, y, W_LABEL, label_of(r->field, r->arg), FB_GREY, FB_CLEAR);
        ui_cells(C_VALUE, y, W_VALUE, buf, FB_WHITE, FB_CLEAR);
        y += UI_LH;
    }
    y += UI_LH / 2;
    wrap_text(RESTART_TEXT, 2, &y, UI_COLS - 4, FB_WHITE);
    if (risky)
        wrap_text(RISK_TEXT, 2, &y, UI_COLS - 4, FB_WHITE);
    y += UI_LH / 2;
    ui_cells(2, y, UI_COLS - 4, "Y: write    N or Esc: not now", FB_WHITE, FB_CLEAR);
}

void ui_settings_draw(void) {
    // The node's settings changed under us (a resync, a write landing)
    build();
    if (sel >= 0) {
        if (sel < top)
            top = sel;
        if (sel >= top + ROWS)
            top = sel - ROWS + 1;
        // Keep the heading above the first field in view, and the notes
        // after the last
        while (top > 0 && top >= sel - 1 && rows[top - 1].kind != R_FIELD && sel - top < ROWS - 1)
            top--;
        bool last = true;
        for (int i = sel + 1; i < nrows; i++)
            last &= rows[i].kind != R_FIELD;
        if (last)
            top = nrows - ROWS;
    }
    if (top > nrows - ROWS)
        top = nrows - ROWS;
    if (top < 0)
        top = 0;

    int y = UI_BODY_Y + 1;
    for (int i = top; i < nrows && i < top + ROWS; i++, y += UI_LH)
        draw_row(&rows[i], y, i == sel);

    if (nrows > ROWS) {
        int t = UI_BODY_Y + 1, h = ROWS * UI_LH;
        int bh = h * ROWS / nrows, by = t + h * top / nrows;
        fb_vline(FB_W - 2, t, h, FB_DIM);
        fb_fill(FB_W - 3, by, 3, bh < 4 ? 4 : bh, FB_WHITE);
    }

    int hy = UI_BODY_Y + UI_BODY_H - UI_LH;
    fb_hline(0, hy - 1, FB_W, FB_DIM);
    ui_cells(0, hy, UI_COLS, hint(), notice ? FB_WHITE : FB_DIM, FB_CLEAR);

    if (confirming)
        draw_confirm();
}

// --- keys ----------------------------------------------------------------------

static void start_edit(int f, int arg) {
    int size;
    const char *t = text_of(&mesh_edit, f, arg, &size);
    snprintf(edit_buf, sizeof edit_buf, "%s", t);
    edit_max = size - 1;
    edit_cursor = (int)strlen(edit_buf);
    editing = true;
}

static void finish_edit(void) {
    int f = rows[sel].field, arg = rows[sel].arg, size;
    // Trim blanks at the ends
    char *s = edit_buf;
    while (*s == ' ')
        s++;
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ')
        s[--n] = '\0';
    if (!n && f != F_CHANNEL) {
        // The node ignores an empty name
        notice = "A name can't be empty.";
        return;
    }
    char *t = text_of(&mesh_edit, f, arg, &size);
    snprintf(t, (size_t)size, "%s", s);
    editing = false;
    mesh_edit_ack();
}

static void edit_key(const kbd_event_t *ev) {
    int len = (int)strlen(edit_buf);
    switch (ev->key) {
    case KBD_ESC: editing = false; return;
    case KBD_ENTER: finish_edit(); return;
    case KBD_LEFT: if (edit_cursor > 0) edit_cursor--; return;
    case KBD_RIGHT: if (edit_cursor < len) edit_cursor++; return;
    case KBD_HOME: edit_cursor = 0; return;
    case KBD_END: edit_cursor = len; return;
    case KBD_BACKSPACE:
        if (edit_cursor > 0) {
            memmove(edit_buf + edit_cursor - 1, edit_buf + edit_cursor,
                    (size_t)(len - edit_cursor + 1));
            edit_cursor--;
        }
        return;
    case KBD_DEL:
        if (edit_cursor < len)
            memmove(edit_buf + edit_cursor, edit_buf + edit_cursor + 1,
                    (size_t)(len - edit_cursor));
        return;
    default: break;
    }
    if (ev->key >= 0x20 && ev->key < 0x7F && !(ev->mods & (KBD_MOD_CTRL | KBD_MOD_ALT))) {
        if (len >= edit_max) {
            notice = "That's as long as it can be.";
            return;
        }
        memmove(edit_buf + edit_cursor + 1, edit_buf + edit_cursor, (size_t)(len - edit_cursor + 1));
        edit_buf[edit_cursor++] = (char)ev->key;
    }
}

static void confirm_key(const kbd_event_t *ev) {
    if (ev->key == 'y' || ev->key == 'Y') {
        confirming = false;
        if (!mesh_edit_write())
            notice = "Not written: the node isn't connected.";
    } else if (ev->key == 'n' || ev->key == 'N' || ev->key == KBD_ESC) {
        confirming = false;
    }
}

bool ui_settings_key(const kbd_event_t *ev) {
    notice = NULL;
    if (confirming) {
        confirm_key(ev);
        return true;
    }
    if (editing) {
        edit_key(ev);
        return true;
    }
    build();
    if (sel < 0)
        return false;

    int f = rows[sel].field, arg = rows[sel].arg;
    bool locked = is_node(f) && !node_editable();
    switch (ev->key) {
    case KBD_UP: move(-1); return true;
    case KBD_DOWN: move(1); return true;
    case KBD_PGUP: for (int i = 0; i < ROWS - 1; i++) move(-1); return true;
    case KBD_PGDN: for (int i = 0; i < ROWS - 1; i++) move(1); return true;
    case KBD_HOME: sel = -1; sel_field = -1; build(); top = 0; return true;
    case KBD_END:
        for (int i = 0; i < nrows; i++)
            move(1);
        return true;
    case KBD_LEFT:
    case KBD_RIGHT:
        if (locked) {
            notice = "Read only while the node isn't connected.";
            return true;
        }
        if (kind_of(f) == K_ENUM || kind_of(f) == K_INT || kind_of(f) == K_BOOL)
            step(f, ev->key == KBD_RIGHT ? 1 : -1);
        return true;
    case KBD_ENTER:
        if (f == A_WRITE) {
            if (mesh_link_conn() == MESH_CONN_UP)
                confirming = true;
            else
                notice = "Not connected to the node.";
        } else if (f == A_DISCARD) {
            mesh_edit = mesh_edit_base;
        } else if (locked) {
            notice = "Read only while the node isn't connected.";
        } else if (kind_of(f) == K_TEXT) {
            start_edit(f, arg);
        } else if (kind_of(f) == K_BOOL) {
            step(f, 1);
        }
        return true;
    default:
        return false;
    }
}
