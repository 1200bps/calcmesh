#include "nodeedit.h"

#include <stdio.h>
#include <string.h>

#include "link.h"
#include "pb_encode.h"

#include "meshtastic/admin.pb.h"

mesh_edit_t mesh_edit;
mesh_edit_t mesh_edit_base;

static mesh_edit_state_t state;
static uint32_t written, mismatch;
static mesh_edit_t sent; // what we wrote, to check against what comes back
static uint32_t write_ms;
static uint32_t write_syncs, write_errors;
static bool resynced; // asked for the config ourselves already

// Static: AdminMessage is ~480 bytes.
static meshtastic_AdminMessage admin;

uint32_t mesh_edit_available(void) {
    uint32_t a = 0;
    if (mesh_db.has_owner)
        a |= MESH_EDIT_OWNER;
    if (mesh_db_config(meshtastic_Config_device_tag))
        a |= MESH_EDIT_DEVICE;
    if (mesh_db_config(meshtastic_Config_position_tag))
        a |= MESH_EDIT_POSITION;
    if (mesh_db_config(meshtastic_Config_power_tag))
        a |= MESH_EDIT_POWER;
    if (mesh_db_config(meshtastic_Config_lora_tag))
        a |= MESH_EDIT_LORA;
    if (mesh_db_config(meshtastic_Config_bluetooth_tag))
        a |= MESH_EDIT_BLUETOOTH;
    return a | (uint32_t)mesh_db.channel_have << 8;
}

static void take(mesh_edit_t *e) {
    const meshtastic_Config *c;
    memset(e, 0, sizeof *e);
    e->owner = mesh_db.owner;
    if ((c = mesh_db_config(meshtastic_Config_device_tag)))
        e->device = c->payload_variant.device;
    if ((c = mesh_db_config(meshtastic_Config_position_tag)))
        e->position = c->payload_variant.position;
    if ((c = mesh_db_config(meshtastic_Config_power_tag)))
        e->power = c->payload_variant.power;
    if ((c = mesh_db_config(meshtastic_Config_lora_tag)))
        e->lora = c->payload_variant.lora;
    if ((c = mesh_db_config(meshtastic_Config_bluetooth_tag)))
        e->bluetooth = c->payload_variant.bluetooth;
    memcpy(e->channels, mesh_db.channel_full, sizeof e->channels);
}

void mesh_edit_load(void) {
    take(&mesh_edit_base);
    mesh_edit = mesh_edit_base;
}

// Sections compare by their encoding, so struct padding and the unused tail
// of strings don't count.
static size_t encode(const pb_msgdesc_t *fields, const void *msg, uint8_t *buf, size_t len) {
    pb_ostream_t os = pb_ostream_from_buffer(buf, len);
    if (!pb_encode(&os, fields, msg))
        return (size_t)-1;
    return os.bytes_written;
}

static bool same(const pb_msgdesc_t *fields, const void *a, const void *b) {
    static uint8_t ba[256], bb[256];
    size_t na = encode(fields, a, ba, sizeof ba);
    size_t nb = encode(fields, b, bb, sizeof bb);
    return na == nb && na != (size_t)-1 && memcmp(ba, bb, na) == 0;
}

static uint32_t differ(const mesh_edit_t *a, const mesh_edit_t *b) {
    uint32_t d = 0;
    if (!same(meshtastic_User_fields, &a->owner, &b->owner))
        d |= MESH_EDIT_OWNER;
    if (!same(meshtastic_Config_DeviceConfig_fields, &a->device, &b->device))
        d |= MESH_EDIT_DEVICE;
    if (!same(meshtastic_Config_PositionConfig_fields, &a->position, &b->position))
        d |= MESH_EDIT_POSITION;
    if (!same(meshtastic_Config_PowerConfig_fields, &a->power, &b->power))
        d |= MESH_EDIT_POWER;
    if (!same(meshtastic_Config_LoRaConfig_fields, &a->lora, &b->lora))
        d |= MESH_EDIT_LORA;
    if (!same(meshtastic_Config_BluetoothConfig_fields, &a->bluetooth, &b->bluetooth))
        d |= MESH_EDIT_BLUETOOTH;
    for (int i = 0; i < MESH_MAX_CHANNELS; i++)
        if (!same(meshtastic_Channel_fields, &a->channels[i], &b->channels[i]))
            d |= MESH_EDIT_CHANNEL(i);
    return d;
}

uint32_t mesh_edit_dirty(void) {
    return differ(&mesh_edit, &mesh_edit_base) & mesh_edit_available();
}

static bool send_config(pb_size_t tag) {
    memset(&admin, 0, sizeof admin);
    admin.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
    meshtastic_Config *c = &admin.set_config;
    c->which_payload_variant = tag;
    switch (tag) {
    case meshtastic_Config_device_tag: c->payload_variant.device = mesh_edit.device; break;
    case meshtastic_Config_position_tag: c->payload_variant.position = mesh_edit.position; break;
    case meshtastic_Config_power_tag: c->payload_variant.power = mesh_edit.power; break;
    case meshtastic_Config_lora_tag: c->payload_variant.lora = mesh_edit.lora; break;
    case meshtastic_Config_bluetooth_tag: c->payload_variant.bluetooth = mesh_edit.bluetooth; break;
    default: return false;
    }
    return mesh_link_send_admin(&admin);
}

static bool send_simple(pb_size_t tag) {
    memset(&admin, 0, sizeof admin);
    admin.which_payload_variant = tag;
    // begin_edit_settings and commit_edit_settings are bools that must be set
    admin.begin_edit_settings = true;
    return mesh_link_send_admin(&admin);
}

bool mesh_edit_write(void) {
    uint32_t d = mesh_edit_dirty();
    if (!d || mesh_link_state() != MESH_LINK_READY)
        return false;
    const mesh_link_stats_t *st = mesh_link_stats();
    write_syncs = st->syncs;
    write_errors = st->admin_errors;

    bool ok = send_simple(meshtastic_AdminMessage_begin_edit_settings_tag);
    if (ok && (d & MESH_EDIT_OWNER)) {
        memset(&admin, 0, sizeof admin);
        admin.which_payload_variant = meshtastic_AdminMessage_set_owner_tag;
        admin.set_owner = mesh_edit.owner;
        ok = mesh_link_send_admin(&admin);
    }
    static const struct {
        uint32_t bit;
        pb_size_t tag;
    } configs[] = {
        {MESH_EDIT_DEVICE, meshtastic_Config_device_tag},
        {MESH_EDIT_POSITION, meshtastic_Config_position_tag},
        {MESH_EDIT_POWER, meshtastic_Config_power_tag},
        {MESH_EDIT_LORA, meshtastic_Config_lora_tag},
        {MESH_EDIT_BLUETOOTH, meshtastic_Config_bluetooth_tag},
    };
    for (size_t i = 0; ok && i < sizeof configs / sizeof configs[0]; i++)
        if (d & configs[i].bit)
            ok = send_config(configs[i].tag);
    for (int i = 0; ok && i < MESH_MAX_CHANNELS; i++)
        if (d & MESH_EDIT_CHANNEL(i)) {
            memset(&admin, 0, sizeof admin);
            admin.which_payload_variant = meshtastic_AdminMessage_set_channel_tag;
            admin.set_channel = mesh_edit.channels[i];
            ok = mesh_link_send_admin(&admin);
        }
    // Commit even after a failure: an open transaction would hold back
    // whatever the node saves next.
    bool committed = send_simple(meshtastic_AdminMessage_commit_edit_settings_tag);
    printf("[edit] wrote sections %04lx%s; waiting for the node to restart\n",
           (unsigned long)d, ok && committed ? "" : " (send failed partway)");

    written = d;
    mismatch = 0;
    sent = mesh_edit;
    write_ms = mesh_link_ms();
    resynced = false;
    state = MESH_EDIT_RESTARTING;
    return true;
}

void mesh_edit_poll(void) {
    const mesh_link_stats_t *st = mesh_link_stats();
    if (state != MESH_EDIT_RESTARTING) {
        // The node's settings changed under us (a resync after desktop mode,
        // say): follow them unless there are edits to keep.
        static uint32_t seen_syncs;
        if (st->syncs != seen_syncs) {
            seen_syncs = st->syncs;
            if (!(differ(&mesh_edit, &mesh_edit_base) & mesh_edit_available()))
                mesh_edit_load();
        }
        return;
    }

    uint32_t t = mesh_link_ms() - write_ms;
    if (st->admin_errors != write_errors && st->syncs == write_syncs) {
        // Refused outright; it won't restart for us
        mesh_edit_load();
        mismatch = written;
        state = MESH_EDIT_REFUSED;
        printf("[edit] the node refused the change (routing error %u)\n",
               st->admin_last_error);
        return;
    }
    if (st->syncs != write_syncs && mesh_link_state() == MESH_LINK_READY) {
        mesh_edit_t now;
        take(&now);
        mismatch = differ(&now, &sent) & written;
        mesh_edit_load();
        state = mismatch ? MESH_EDIT_MISMATCH : MESH_EDIT_DONE;
        printf("[edit] node back; %s\n",
               mismatch ? "some settings didn't read back as written" : "all settings confirmed");
        return;
    }
    if (t >= MESH_EDIT_TIMEOUT_MS) {
        mesh_edit_load();
        mismatch = written;
        state = MESH_EDIT_NO_RESTART;
        printf("[edit] the node hasn't come back\n");
        return;
    }
    // The link notices a restart when the node says so; if it didn't, ask.
    if (t >= MESH_EDIT_RESYNC_MS && !resynced && mesh_link_state() == MESH_LINK_READY) {
        resynced = true;
        mesh_link_request_config();
    }
}

mesh_edit_state_t mesh_edit_state(void) { return state; }
uint32_t mesh_edit_written(void) { return written; }
uint32_t mesh_edit_mismatch(void) { return mismatch; }

void mesh_edit_ack(void) {
    if (state != MESH_EDIT_RESTARTING)
        state = MESH_EDIT_IDLE;
}
