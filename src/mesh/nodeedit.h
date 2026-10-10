// Changing the node's own settings: owner names, LoRa, channels, position,
// power and Bluetooth, through admin messages (admin.proto) sent to our own
// node over the serial link.
//
// mesh_edit is a working copy that the Settings tab edits. Nothing reaches
// the node until mesh_edit_write(), which sends every changed section in one
// edit transaction (begin_edit_settings ... commit_edit_settings). The node
// saves them and restarts; we wait for it to come back, re-read everything,
// and check that what it now reports matches what we wrote.
//
// Each section goes out whole, copied from what the node sent, so fields we
// don't show (ignore_incoming, PSKs, is_licensed and so on) are written back
// as they were.
//
// It can also be pointed at another node (mesh_edit_select), whose settings
// are then fetched and written over the mesh instead (remoteadmin.h).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "nodedb.h"

// Sections, as bits. Channel n is MESH_EDIT_CHANNEL(n).
enum {
    MESH_EDIT_OWNER = 1u << 0,
    MESH_EDIT_DEVICE = 1u << 1,
    MESH_EDIT_POSITION = 1u << 2,
    MESH_EDIT_POWER = 1u << 3,
    MESH_EDIT_LORA = 1u << 4,
    MESH_EDIT_BLUETOOTH = 1u << 5,
};
#define MESH_EDIT_CHANNEL(n) (1u << (8 + (n)))
#define MESH_EDIT_CHANNELS (0xFFu << 8)

typedef struct {
    meshtastic_User owner;
    meshtastic_Config_DeviceConfig device;
    meshtastic_Config_PositionConfig position;
    meshtastic_Config_PowerConfig power;
    meshtastic_Config_LoRaConfig lora;
    meshtastic_Config_BluetoothConfig bluetooth;
    meshtastic_Channel channels[MESH_MAX_CHANNELS];
} mesh_edit_t;

// The working copy, and the node's settings it was taken from.
extern mesh_edit_t mesh_edit;
extern mesh_edit_t mesh_edit_base;

// Which node: 0 for our own, else another node's number. Switching drops
// any edits; false (and no switch) while a write or fetch is under way.
uint32_t mesh_edit_target(void);
bool mesh_edit_select(uint32_t node);
// Another node: ask it for all its settings (again). False if busy, if
// there's no link, or if it's our own node (which sends them by itself).
bool mesh_edit_fetch(void);

// Sections the node has sent us; only those can be edited or written.
uint32_t mesh_edit_available(void);
// Take a fresh copy of the node's settings into both, dropping any edits.
void mesh_edit_load(void);
// Sections where the working copy differs from the base.
uint32_t mesh_edit_dirty(void);

typedef enum {
    MESH_EDIT_IDLE,
    MESH_EDIT_RESTARTING, // written; waiting for the node to restart and resync
    MESH_EDIT_DONE,       // back, and everything we wrote reads back the same
    MESH_EDIT_MISMATCH,   // back, but mesh_edit_mismatch() sections differ
    MESH_EDIT_NO_RESTART, // the node never came back with its settings
    MESH_EDIT_REFUSED,    // the node answered our admin messages with an error
} mesh_edit_state_t;

// Write the dirty sections. False (and nothing sent) if there are none, or
// the link isn't up (no node, still syncing, or desktop mode has it).
bool mesh_edit_write(void);
// Call regularly: follows the node through its restart.
void mesh_edit_poll(void);
mesh_edit_state_t mesh_edit_state(void);
// What was written last time, and which of it didn't read back the same.
uint32_t mesh_edit_written(void);
uint32_t mesh_edit_mismatch(void);
// For REFUSED and NO_RESTART: a meshtastic_Routing_Error, or RA_ERR_*.
uint8_t mesh_edit_error(void);
// Back to IDLE once the result has been seen.
void mesh_edit_ack(void);

// If the node hasn't resynced this long after the write, ask it again.
#define MESH_EDIT_RESYNC_MS 15000
// Give up waiting for it after this long.
#define MESH_EDIT_TIMEOUT_MS 90000
