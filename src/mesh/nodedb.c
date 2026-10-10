#include "nodedb.h"

#include <stdio.h>
#include <string.h>

mesh_db_t mesh_db;

#define COPY_STR(dst, src) snprintf((dst), sizeof(dst), "%s", (src))

void mesh_db_clear(void) { memset(&mesh_db, 0, sizeof mesh_db); }

void mesh_db_set_my_info(const meshtastic_MyNodeInfo *mi) {
    mesh_db.my_node_num = mi->my_node_num;
    mesh_db.reboot_count = mi->reboot_count;
    mesh_db.nodedb_count = mi->nodedb_count;
    COPY_STR(mesh_db.pio_env, mi->pio_env);
}

void mesh_db_set_metadata(const meshtastic_DeviceMetadata *md) {
    COPY_STR(mesh_db.firmware_version, md->firmware_version);
    mesh_db.hw_model = (uint16_t)md->hw_model;
}

void mesh_db_set_config(const meshtastic_Config *cfg) {
    pb_size_t tag = cfg->which_payload_variant;
    if (tag < MESH_CONFIG_SLOTS) {
        mesh_db.config[tag] = *cfg;
        mesh_db.config_have |= 1u << tag;
    }
    if (tag != meshtastic_Config_lora_tag)
        return;
    const meshtastic_Config_LoRaConfig *l = &cfg->payload_variant.lora;
    mesh_db.has_lora = true;
    mesh_db.use_preset = l->use_preset;
    mesh_db.modem_preset = (uint8_t)l->modem_preset;
    mesh_db.region = (uint8_t)l->region;
    mesh_db.hop_limit = (uint8_t)l->hop_limit;
}

const meshtastic_Config *mesh_db_config(pb_size_t tag) {
    return tag < MESH_CONFIG_SLOTS && (mesh_db.config_have >> tag & 1) ? &mesh_db.config[tag]
                                                                        : NULL;
}

void mesh_db_set_module_config(const meshtastic_ModuleConfig *mc) {
    pb_size_t tag = mc->which_payload_variant;
    if (tag >= MESH_MODULE_SLOTS)
        return;
    mesh_db.module[tag] = *mc;
    mesh_db.module_have |= 1u << tag;
}

const meshtastic_ModuleConfig *mesh_db_module(pb_size_t tag) {
    return tag < MESH_MODULE_SLOTS && (mesh_db.module_have >> tag & 1) ? &mesh_db.module[tag]
                                                                        : NULL;
}

void mesh_db_set_channel(const meshtastic_Channel *ch) {
    if (ch->index < 0 || ch->index >= MESH_MAX_CHANNELS)
        return;
    mesh_db.channel_full[ch->index] = *ch;
    mesh_db.channel_have |= (uint8_t)(1u << ch->index);
    mesh_channel_t *c = &mesh_db.channels[ch->index];
    memset(c, 0, sizeof *c);
    c->role = (uint8_t)ch->role;
    if (ch->has_settings) {
        const meshtastic_ChannelSettings *s = &ch->settings;
        COPY_STR(c->name, s->name);
        c->psk_len = (uint8_t)s->psk.size;
        if (s->psk.size == 1)
            c->psk_index = s->psk.bytes[0];
        c->psk_hash = 2166136261u;
        for (size_t i = 0; i < s->psk.size; i++)
            c->psk_hash = (c->psk_hash ^ s->psk.bytes[i]) * 16777619u;
        c->uplink = s->uplink_enabled;
        c->downlink = s->downlink_enabled;
    }
}

mesh_node_t *mesh_db_find(uint32_t num) {
    for (int i = 0; i < mesh_db.node_count; i++)
        if (mesh_db.nodes[i].num == num)
            return &mesh_db.nodes[i];
    return NULL;
}

// Slot for a node we haven't seen: append, or when full reuse the
// least recently heard entry that isn't us or a favourite.
static mesh_node_t *alloc_node(void) {
    if (mesh_db.node_count < MESH_MAX_NODES)
        return &mesh_db.nodes[mesh_db.node_count++];
    mesh_node_t *victim = NULL;
    for (int i = 0; i < MESH_MAX_NODES; i++) {
        mesh_node_t *n = &mesh_db.nodes[i];
        if (n->num == mesh_db.my_node_num || n->is_favorite)
            continue;
        if (!victim || n->last_heard < victim->last_heard)
            victim = n;
    }
    if (victim)
        mesh_db.evicted++;
    return victim;
}

mesh_node_t *mesh_db_upsert_node(const meshtastic_NodeInfo *ni) {
    mesh_node_t *n = mesh_db_find(ni->num);
    if (!n) {
        n = alloc_node();
        if (!n)
            return NULL;
    }
    memset(n, 0, sizeof *n);
    n->num = ni->num;
    if (ni->has_user) {
        n->has_user = true;
        COPY_STR(n->id, ni->user.id);
        COPY_STR(n->long_name, ni->user.long_name);
        COPY_STR(n->short_name, ni->user.short_name);
        n->hw_model = (uint16_t)ni->user.hw_model;
        n->role = (uint8_t)ni->user.role;
    }
    if (!n->id[0])
        snprintf(n->id, sizeof n->id, "!%08lx", (unsigned long)ni->num);
    n->hops_away = ni->has_hops_away ? ni->hops_away : MESH_HOPS_UNKNOWN;
    n->battery = MESH_BATTERY_UNKNOWN;
    if (ni->has_device_metrics && ni->device_metrics.has_battery_level)
        n->battery = ni->device_metrics.battery_level > MESH_BATTERY_POWERED
                         ? MESH_BATTERY_POWERED
                         : (uint8_t)ni->device_metrics.battery_level;
    if (ni->has_position && ni->position.has_latitude_i &&
        ni->position.has_longitude_i) {
        n->has_position = true;
        n->latitude_i = ni->position.latitude_i;
        n->longitude_i = ni->position.longitude_i;
        n->altitude = ni->position.has_altitude ? ni->position.altitude : 0;
    }
    if (ni->has_user && ni->num == mesh_db.my_node_num && ni->num) {
        mesh_db.owner = ni->user;
        mesh_db.has_owner = true;
    }
    n->via_mqtt = ni->via_mqtt;
    n->is_favorite = ni->is_favorite;
    n->snr = ni->snr;
    n->last_heard = ni->last_heard;
    return n;
}

const char *mesh_db_preset_name(uint8_t preset) {
    static const char *const names[] = {
        [meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST] = "LongFast",
        [meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW] = "LongSlow",
        [meshtastic_Config_LoRaConfig_ModemPreset_VERY_LONG_SLOW] = "VLongSlow",
        [meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_SLOW] = "MediumSlow",
        [meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST] = "MediumFast",
        [meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW] = "ShortSlow",
        [meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST] = "ShortFast",
        [meshtastic_Config_LoRaConfig_ModemPreset_LONG_MODERATE] = "LongMod",
        [meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO] = "ShortTurbo",
        [meshtastic_Config_LoRaConfig_ModemPreset_LONG_TURBO] = "LongTurbo",
        [meshtastic_Config_LoRaConfig_ModemPreset_LITE_FAST] = "LiteFast",
        [meshtastic_Config_LoRaConfig_ModemPreset_LITE_SLOW] = "LiteSlow",
        [meshtastic_Config_LoRaConfig_ModemPreset_NARROW_FAST] = "NarrowFast",
        [meshtastic_Config_LoRaConfig_ModemPreset_NARROW_SLOW] = "NarrowSlow",
        [meshtastic_Config_LoRaConfig_ModemPreset_TINY_FAST] = "TinyFast",
        [meshtastic_Config_LoRaConfig_ModemPreset_TINY_SLOW] = "TinySlow",
        [meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_TURBO] = "MediumTurbo",
    };
    if (preset < sizeof names / sizeof names[0] && names[preset])
        return names[preset];
    return "Custom";
}

const char *mesh_db_channel_name(int index) {
    if (index < 0 || index >= MESH_MAX_CHANNELS)
        return "";
    const mesh_channel_t *c = &mesh_db.channels[index];
    if (c->name[0])
        return c->name;
    if (c->role == meshtastic_Channel_Role_PRIMARY) {
        if (!mesh_db.has_lora)
            return "(primary)";
        return mesh_db.use_preset ? mesh_db_preset_name(mesh_db.modem_preset)
                                  : "Custom";
    }
    return "";
}

static const char *role_name(uint8_t role) {
    switch (role) {
    case meshtastic_Channel_Role_PRIMARY:
        return "primary";
    case meshtastic_Channel_Role_SECONDARY:
        return "secondary";
    default:
        return "disabled";
    }
}

static void psk_desc(const mesh_channel_t *c, char *out, size_t n) {
    switch (c->psk_len) {
    case 0:
        snprintf(out, n, "none");
        break;
    case 1:
        if (c->psk_index == 0)
            snprintf(out, n, "none");
        else if (c->psk_index == 1)
            snprintf(out, n, "default");
        else
            snprintf(out, n, "simple%u", c->psk_index - 1);
        break;
    case 16:
        snprintf(out, n, "AES128");
        break;
    case 32:
        snprintf(out, n, "AES256");
        break;
    default:
        snprintf(out, n, "%u bytes?", c->psk_len);
    }
}

// Format "how long ago" against a reference clock. The Pico has no RTC, so the
// caller uses the newest last_heard in the table as "now".
static void ago(uint32_t then, uint32_t now, char *out, size_t n) {
    if (!then) {
        snprintf(out, n, "never");
        return;
    }
    uint32_t d = now > then ? now - then : 0;
    if (d < 60)
        snprintf(out, n, "%lus", (unsigned long)d);
    else if (d < 3600)
        snprintf(out, n, "%lum", (unsigned long)(d / 60));
    else if (d < 86400)
        snprintf(out, n, "%luh", (unsigned long)(d / 3600));
    else
        snprintf(out, n, "%lud", (unsigned long)(d / 86400));
}

void mesh_db_dump(void) {
    const mesh_node_t *me = mesh_db_find(mesh_db.my_node_num);
    printf("== my node !%08lx", (unsigned long)mesh_db.my_node_num);
    if (me && me->has_user)
        printf(" \"%s\" (%s)", me->long_name, me->short_name);
    printf("\n   fw %s  env %s  hw %u  reboots %lu\n",
           mesh_db.firmware_version[0] ? mesh_db.firmware_version : "?",
           mesh_db.pio_env[0] ? mesh_db.pio_env : "?", mesh_db.hw_model,
           (unsigned long)mesh_db.reboot_count);
    if (mesh_db.has_lora)
        printf("   lora region %u, %s\n", mesh_db.region,
               mesh_db.use_preset ? mesh_db_preset_name(mesh_db.modem_preset)
                                  : "custom modem settings");

    printf("== channels\n");
    for (int i = 0; i < MESH_MAX_CHANNELS; i++) {
        const mesh_channel_t *c = &mesh_db.channels[i];
        if (c->role == meshtastic_Channel_Role_DISABLED)
            continue;
        char psk[16];
        psk_desc(c, psk, sizeof psk);
        printf("  %d  %-9s  %-12s  psk %s%s%s\n", i, role_name(c->role),
               mesh_db_channel_name(i), psk, c->uplink ? "  up" : "",
               c->downlink ? "  down" : "");
    }

    uint32_t now = 0;
    for (int i = 0; i < mesh_db.node_count; i++)
        if (mesh_db.nodes[i].last_heard > now)
            now = mesh_db.nodes[i].last_heard;

    printf("== nodes: %u in table, node reported %u", mesh_db.node_count,
           mesh_db.nodedb_count);
    if (mesh_db.evicted)
        printf(", %lu evicted (table full)", (unsigned long)mesh_db.evicted);
    printf("\n  %-10s %-4s %-24s %4s %6s %6s %4s  %s\n", "id", "shrt", "name",
           "hops", "snr", "heard", "batt", "position");
    for (int i = 0; i < mesh_db.node_count; i++) {
        const mesh_node_t *n = &mesh_db.nodes[i];
        char hops[5], heard[8], batt[5], pos[40];
        if (n->num == mesh_db.my_node_num)
            snprintf(hops, sizeof hops, "me");
        else if (n->hops_away == MESH_HOPS_UNKNOWN)
            snprintf(hops, sizeof hops, "?");
        else
            snprintf(hops, sizeof hops, "%u", n->hops_away);
        ago(n->last_heard, now, heard, sizeof heard);
        if (n->battery == MESH_BATTERY_UNKNOWN)
            snprintf(batt, sizeof batt, "-");
        else if (n->battery == MESH_BATTERY_POWERED)
            snprintf(batt, sizeof batt, "ext");
        else
            snprintf(batt, sizeof batt, "%u%%", n->battery);
        if (n->has_position)
            snprintf(pos, sizeof pos, "%.5f,%.5f", n->latitude_i / 1e7,
                     n->longitude_i / 1e7);
        else
            pos[0] = '\0';
        printf("  %-10s %-4s %-24.24s %4s %6.1f %6s %4s  %s%s%s\n", n->id,
               n->short_name, n->long_name, hops, (double)n->snr, heard, batt,
               pos, n->via_mqtt ? " mqtt" : "", n->is_favorite ? " fav" : "");
    }
}
