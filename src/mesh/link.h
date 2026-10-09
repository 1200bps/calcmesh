// Client side of the Meshtastic serial API: frames bytes from the node into
// FromRadio messages, runs the want_config handshake, and keeps mesh_db
// (nodedb.h) up to date. Transport-agnostic: the caller supplies a write
// function and a millisecond clock, feeds received bytes in with
// mesh_link_rx(), and calls mesh_link_poll() regularly.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    // Send bytes to the node. Must send all of them (block if needed).
    void (*write)(const uint8_t *data, size_t len, void *ctx);
    uint32_t (*now_ms)(void *ctx);
    void *ctx;
    // Mixed into want_config nonces so they differ across boots (optional).
    uint32_t seed;
} mesh_link_io_t;

typedef enum {
    MESH_LINK_IDLE,       // nothing requested yet
    MESH_LINK_WAKING,     // sent the wake preamble, about to ask for config
    MESH_LINK_CONFIGURING, // want_config sent, receiving the dump
    MESH_LINK_READY,      // config_complete received; mesh_db is populated
} mesh_link_state_t;

typedef struct {
    uint32_t frames;     // FromRadio frames decoded
    uint32_t bad_frames; // frames that failed to decode
    uint32_t requests;   // want_config attempts
} mesh_link_stats_t;

void mesh_link_init(const mesh_link_io_t *io);

// Start (or restart) the handshake. mesh_db is cleared once the node starts
// answering.
void mesh_link_request_config(void);

void mesh_link_rx(const uint8_t *data, size_t len);
void mesh_link_poll(void);

mesh_link_state_t mesh_link_state(void);
const mesh_link_stats_t *mesh_link_stats(void);

// Called once each time a handshake completes. Defaults to mesh_db_dump().
void mesh_link_on_ready(void (*cb)(void));
