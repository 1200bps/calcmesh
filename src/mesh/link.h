// Client side of the Meshtastic serial API: frames bytes from the node into
// FromRadio messages, runs the want_config handshake, keeps mesh_db
// (nodedb.h) up to date, and sends and receives text messages (msgs.h).
// Transport-agnostic: the caller supplies a write function and a millisecond
// clock, feeds received bytes in with mesh_link_rx(), and calls
// mesh_link_poll() regularly.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "msgs.h"

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
    MESH_LINK_PASSIVE,    // someone else is driving the node; listen only
} mesh_link_state_t;

typedef struct {
    uint32_t frames;     // FromRadio frames decoded
    uint32_t bad_frames; // frames that failed to decode
    uint32_t requests;   // want_config attempts
    uint32_t texts_rx;   // text messages received
    uint32_t texts_tx;   // text messages sent
} mesh_link_stats_t;

typedef enum {
    MESH_SEND_OK,
    MESH_SEND_NOT_READY,  // handshake not finished, or desktop mode has the node
    MESH_SEND_EMPTY,
    MESH_SEND_TOO_LONG,   // over MESH_MSG_SEND_MAX bytes
    MESH_SEND_NO_CHANNEL, // that channel index isn't enabled on the node
    MESH_SEND_FAILED,     // couldn't encode or store it
} mesh_send_result_t;

void mesh_link_init(const mesh_link_io_t *io);

// Start (or restart) the handshake. mesh_db is cleared once the node starts
// answering.
void mesh_link_request_config(void);

// Passive: never write to the node (no handshake, retries or heartbeats),
// but keep decoding what it sends so mesh_db follows along. Leaving passive
// mode restarts the handshake, since whoever was driving may have left the
// node's API session in any state.
void mesh_link_set_passive(bool passive);

void mesh_link_rx(const uint8_t *data, size_t len);
void mesh_link_poll(void);

mesh_link_state_t mesh_link_state(void);
const mesh_link_stats_t *mesh_link_stats(void);

// Send text (UTF-8, len bytes, need not be NUL-terminated) to a channel or as
// a DM, asking for an ack. reply_id, if not 0, marks it as a reply to that
// packet. On MESH_SEND_OK the message is in the store with status QUEUED and
// *out (if out isn't NULL) points at it; the status moves on as the node
// reports back.
mesh_send_result_t mesh_link_send_text(mesh_conv_t conv, const char *text,
                                       size_t len, uint32_t reply_id,
                                       mesh_msg_t **out);
const char *mesh_link_send_result_name(mesh_send_result_t r);

// The node's clock (epoch seconds), extrapolated from the newest received
// packet that carried one; 0 until then.
uint32_t mesh_link_epoch(void);

// Called once each time a handshake completes. Defaults to mesh_db_dump().
void mesh_link_on_ready(void (*cb)(void));
