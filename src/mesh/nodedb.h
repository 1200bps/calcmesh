// In-memory tables built from the node's want_config dump and kept current
// from later FromRadio traffic: who we are, the mesh nodes we know about, and
// the channel table.
//
// The node only sends its NodeDB during a handshake. After that, as the
// official clients do, we follow the packets it passes on: any packet from a
// node we don't know adds it, NodeInfo packets fill in its name and key, and
// link.c asks for the node list again now and then (MESH_NODES_REFRESH_MS)
// to catch what we missed. The table survives a re-handshake, and the SD
// card keeps a copy (sd/nodecache.h); a node our node didn't list in its
// latest dump is kept, marked cached.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "meshtastic/mesh.pb.h"

#ifndef MESH_MAX_NODES
#define MESH_MAX_NODES 256
#endif
#define MESH_MAX_CHANNELS 8

// Payload tags run 1..10 for Config and 1..17 for ModuleConfig.
#define MESH_CONFIG_SLOTS 12
#define MESH_MODULE_SLOTS 20

#define MESH_HOPS_UNKNOWN 0xFF
#define MESH_CHANNEL_UNKNOWN 0xFF // to mesh_db_heard(): leave it as it is
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
    bool has_key; // our node has its public key, so it can be sent DMs, metrics
                  // requests and remote admin (all PKI-encrypted)
    bool cached;  // our node didn't list it last time it sent its node list,
                  // and we haven't heard it since: kept from earlier (or from
                  // the SD card); our node knows nothing about it
    bool dumped;  // seen in (or heard during) the node list being received
    uint8_t channel; // channel index we hear it on, for asking it for NodeInfo
    float snr;
    uint32_t last_heard; // node's clock, epoch seconds; 0 if never
    int32_t latitude_i;  // 1e-7 degrees
    int32_t longitude_i;
    int32_t altitude; // metres
    uint32_t position_time; // when the fix was taken, epoch seconds; 0 if not said
    uint8_t precision_bits; // of lat/lon kept, 32 = full; 0 if not said
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

    // Everything the node sent about its settings, as it sent it, for the
    // Settings tab to show, edit and write back whole (nodeedit.h).
    // config[] and module[] are indexed by payload tag (meshtastic_Config_lora_tag
    // and so on); bit n of *_have says whether slot n arrived.
    bool has_owner;
    meshtastic_User owner; // our node's User, from its NodeInfo
    uint32_t config_have;
    meshtastic_Config config[MESH_CONFIG_SLOTS];
    uint32_t module_have;
    meshtastic_ModuleConfig module[MESH_MODULE_SLOTS];
    uint8_t channel_have; // bit n: channel n arrived
    meshtastic_Channel channel_full[MESH_MAX_CHANNELS];

    uint16_t node_count;
    uint32_t evicted; // nodes pushed out because the table was full
    uint32_t nodes_owner;   // node number of the radio that told us about them
    uint32_t nodes_version; // bumped whenever a node's entry changes
    mesh_node_t nodes[MESH_MAX_NODES];
} mesh_db_t;

extern mesh_db_t mesh_db;

void mesh_db_clear(void);

// A handshake's dump is starting (full: everything but the node table is
// forgotten; nodes_only: just the node list) and has ended. Nodes the dump
// didn't list, and that weren't heard meanwhile, become cached.
void mesh_db_dump_begin(bool nodes_only);
void mesh_db_dump_end(void);
void mesh_db_set_my_info(const meshtastic_MyNodeInfo *mi);
void mesh_db_set_metadata(const meshtastic_DeviceMetadata *md);
void mesh_db_set_config(const meshtastic_Config *cfg);
void mesh_db_set_channel(const meshtastic_Channel *ch);
void mesh_db_set_module_config(const meshtastic_ModuleConfig *mc);
// A config or module config section, or NULL if the node hasn't sent it.
const meshtastic_Config *mesh_db_config(pb_size_t tag);
const meshtastic_ModuleConfig *mesh_db_module(pb_size_t tag);
mesh_node_t *mesh_db_upsert_node(const meshtastic_NodeInfo *ni);
mesh_node_t *mesh_db_find(uint32_t num);

// A POSITION_APP packet from a node: store its fix (adding the node if we
// didn't know it). A position without coordinates, or at 0,0 (what firmware
// sends with no fix), changes nothing. Returns the node, or NULL.
mesh_node_t *mesh_db_set_position(uint32_t num, const meshtastic_Position *pos);

// Any decoded packet from another node, as our node's NodeDB::updateFrom
// treats it: adds the node if we didn't know it, and keeps its last heard
// time (rx_time, node's clock; 0 if none), SNR, hops away (MESH_HOPS_UNKNOWN
// if the packet can't say) and channel (or MESH_CHANNEL_UNKNOWN) current. Returns the node, or NULL if
// the table is full of nodes that can't be evicted.
mesh_node_t *mesh_db_heard(uint32_t num, uint32_t rx_time, float snr, uint8_t hops,
                           uint8_t channel, bool via_mqtt);

// A NODEINFO_APP packet from a node: its name, hardware, role and public key.
mesh_node_t *mesh_db_set_user(uint32_t num, const meshtastic_User *u);

// A node from the SD card copy, added as cached if we don't know it already
// (whatever we have is newer). NULL if not added.
mesh_node_t *mesh_db_restore_node(const mesh_node_t *saved);

// Name to show for a channel: its own name, or for an unnamed primary the
// modem preset name ("LongFast"), as the official clients do.
const char *mesh_db_channel_name(int index);
const char *mesh_db_preset_name(uint8_t preset);

// Print the tables as text (stdout; USB serial on the Pico).
void mesh_db_dump(void);
