// In-memory tables built from the node's want_config dump and kept current
// from later FromRadio traffic: who we are, the mesh nodes we know about, and
// the channel table.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "meshtastic/mesh.pb.h"

#ifndef MESH_MAX_NODES
#define MESH_MAX_NODES 256
#endif
#define MESH_MAX_CHANNELS 8

#define MESH_HOPS_UNKNOWN 0xFF
#define MESH_BATTERY_UNKNOWN 0xFF
#define MESH_BATTERY_POWERED 101 // firmware reports >100 for external power

typedef struct {
    uint32_t num;
    char id[16];
    char long_name[40];
    char short_name[5];
    uint16_t hw_model;
    uint8_t role;
    uint8_t hops_away; // MESH_HOPS_UNKNOWN if not reported
    uint8_t battery;   // percent, MESH_BATTERY_POWERED, or MESH_BATTERY_UNKNOWN
    bool has_user;
    bool has_position;
    bool via_mqtt;
    bool is_favorite;
    float snr;
    uint32_t last_heard; // node's clock, epoch seconds; 0 if never
    int32_t latitude_i;  // 1e-7 degrees
    int32_t longitude_i;
    int32_t altitude; // metres
} mesh_node_t;

typedef struct {
    uint8_t role; // meshtastic_Channel_Role
    char name[12];
    uint8_t psk_len; // 0 none, 1 well-known shorthand, 16 AES128, 32 AES256
    uint8_t psk_index; // value of the 1-byte shorthand (1 = default key)
    uint32_t psk_hash; // FNV-1a of the key bytes as the node sent them, to tell
                       // channels apart without keeping the key
    bool uplink;
    bool downlink;
} mesh_channel_t;

typedef struct {
    // From MyNodeInfo / DeviceMetadata
    uint32_t my_node_num;
    uint32_t reboot_count;
    uint16_t nodedb_count; // what the node says it will send
    char firmware_version[18];
    char pio_env[40];
    uint16_t hw_model;

    // From Config.lora, needed to name an unnamed primary channel
    bool has_lora;
    bool use_preset;
    uint8_t modem_preset;
    uint8_t region;
    uint8_t hop_limit; // 0 if the node didn't say

    mesh_channel_t channels[MESH_MAX_CHANNELS];

    uint16_t node_count;
    uint32_t evicted; // nodes pushed out because the table was full
    mesh_node_t nodes[MESH_MAX_NODES];
} mesh_db_t;

extern mesh_db_t mesh_db;

void mesh_db_clear(void);
void mesh_db_set_my_info(const meshtastic_MyNodeInfo *mi);
void mesh_db_set_metadata(const meshtastic_DeviceMetadata *md);
void mesh_db_set_config(const meshtastic_Config *cfg);
void mesh_db_set_channel(const meshtastic_Channel *ch);
mesh_node_t *mesh_db_upsert_node(const meshtastic_NodeInfo *ni);
mesh_node_t *mesh_db_find(uint32_t num);

// Name to show for a channel: its own name, or for an unnamed primary the
// modem preset name ("LongFast"), as the official clients do.
const char *mesh_db_channel_name(int index);
const char *mesh_db_preset_name(uint8_t preset);

// Print the tables as text (stdout; USB serial on the Pico).
void mesh_db_dump(void);
