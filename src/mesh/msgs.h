// Text message store: every text message sent or received, grouped into
// conversations (one per channel index, one per DM peer), held in RAM in a
// fixed pool. When the pool is full the oldest message of the busiest
// conversation goes, so a chatty channel can't push a quiet DM out.
//
// The link (link.h) fills it: mesh_link_send_text() adds outgoing messages and
// tracks their delivery, and received TEXT_MESSAGE_APP packets land here. The
// UI only reads, plus mesh_msgs_mark_read(). Single-threaded: call everything
// from the same loop that runs the link.
//
// Persistence (SD card, later) hangs off mesh_msgs_set_observer(): an
// observer sees every message as it is added or changes status and can log
// it; at boot it replays what it saved through mesh_msgs_restore().
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Messages held across all conversations (~270 bytes each).
#ifndef MESH_MSG_POOL
#define MESH_MSG_POOL 64
#endif
// DM conversations tracked at once; the least recently active one is dropped
// (with its messages) to make room for a new peer.
#ifndef MESH_MAX_DMS
#define MESH_MAX_DMS 16
#endif
// Largest text a packet can carry (Data.payload).
#define MESH_MSG_TEXT_MAX 233
// Longest text we send, as the official clients cap it: leaves room for DM
// encryption overhead.
#define MESH_MSG_SEND_MAX 200

#define MESH_ADDR_BROADCAST 0xFFFFFFFFu

// A conversation: a channel (peer == 0) or a DM with peer. Node number 0 is
// never a real node.
typedef struct {
    uint32_t peer;
    uint8_t channel; // channel index; meaningless for DMs
} mesh_conv_t;

#define MESH_CONV_CHANNEL(ch) ((mesh_conv_t){.peer = 0, .channel = (ch)})
#define MESH_CONV_DM(node) ((mesh_conv_t){.peer = (node), .channel = 0})
#define MESH_CONV_IS_DM(c) ((c).peer != 0)

typedef enum {
    MESH_MSG_RECEIVED,  // incoming
    MESH_MSG_QUEUED,    // outgoing: written to the node, nothing heard back yet
    MESH_MSG_SENT,      // the node took it into its transmit queue
    MESH_MSG_RELAYED,   // heard another node rebroadcast it (all a channel
                        // message gets; for a DM, delivery isn't confirmed yet)
    MESH_MSG_DELIVERED, // DM: the destination acknowledged it
    MESH_MSG_FAILED,    // see error
} mesh_msg_status_t;

typedef struct {
    uint32_t id;       // packet id (ours for outgoing; sender's for incoming)
    uint32_t from;     // sender node number (our own for outgoing)
    uint32_t to;       // node number or MESH_ADDR_BROADCAST
    uint32_t seq;      // store-wide insertion order, never reused
    uint32_t rx_time;  // epoch seconds by the node's clock; 0 if unknown
    uint32_t local_ms; // our clock when it was stored
    uint32_t reply_id; // packet id this replies or reacts to; 0 if none
    float snr;         // incoming only
    int16_t rssi;      // incoming only; 0 if not reported
    uint8_t hops;      // hops travelled, incoming only; 0xFF if unknown
    uint8_t channel;   // channel index it went out / came in on
    uint8_t status;    // mesh_msg_status_t
    uint8_t error;     // meshtastic_Routing_Error when status is FAILED
    bool emoji;        // a reaction to reply_id rather than a message
    bool via_mqtt;
    uint8_t len;       // bytes of text, excluding the terminator
    char text[MESH_MSG_TEXT_MAX + 1]; // UTF-8, NUL-terminated
} mesh_msg_t;

typedef struct {
    mesh_conv_t conv;
    uint16_t count;     // messages held
    uint16_t unread;    // received since the last mesh_msgs_mark_read()
    uint32_t last_seq;  // seq of its newest message; orders conversations
} mesh_conv_info_t;

void mesh_msgs_clear(void);

// Bumped by every change (new message, status change, read marker). A UI can
// poll it and redraw when it moves.
uint32_t mesh_msgs_version(void);

// Conversations that hold messages, most recently active first; i in
// [0, mesh_msgs_conv_count()). Channels with no traffic aren't listed, so a
// channel tab should take its list from mesh_db and use mesh_msgs_conv().
int mesh_msgs_conv_count(void);
const mesh_conv_info_t *mesh_msgs_conv_at(int i);
// NULL if that conversation has no messages.
const mesh_conv_info_t *mesh_msgs_conv(mesh_conv_t conv);

// Messages of one conversation, newest first: i == 0 is the latest, up to
// mesh_msgs_count(conv) - 1. Pointers stay valid until that message is
// evicted; don't keep them across a store change (see mesh_msgs_version()).
int mesh_msgs_count(mesh_conv_t conv);
const mesh_msg_t *mesh_msgs_recent(mesh_conv_t conv, int i);

void mesh_msgs_mark_read(mesh_conv_t conv);
uint32_t mesh_msgs_unread_total(void);

// The conversation a message belongs to.
mesh_conv_t mesh_msgs_conv_of(const mesh_msg_t *m);

// "delivered", "failed: no route", ...: short text for a status line.
const char *mesh_msgs_status_name(const mesh_msg_t *m);

// Print a message on one line (stdout), with sender names from mesh_db.
void mesh_msgs_print(const mesh_msg_t *m);

// ---- for the link and persistence ---------------------------------------------

typedef struct {
    // A message was stored (new, or replayed by mesh_msgs_restore()).
    void (*added)(const mesh_msg_t *m, void *ctx);
    // An outgoing message's status changed.
    void (*updated)(const mesh_msg_t *m, void *ctx);
    void *ctx;
} mesh_msgs_observer_t;

// Clock for mesh_msg_t.local_ms (the link installs its own).
void mesh_msgs_set_clock(uint32_t (*now_ms)(void));

// NULL to remove. The observer isn't called for restored messages.
void mesh_msgs_set_observer(const mesh_msgs_observer_t *obs);

// Store a copy of m into conversation conv (seq and local_ms are assigned
// here). Returns the stored message, or NULL if it duplicates one already
// held (same sender and packet id).
mesh_msg_t *mesh_msgs_add(mesh_conv_t conv, const mesh_msg_t *m);
// Same, for messages loaded back from storage: keeps unread at 0 and doesn't
// notify the observer.
mesh_msg_t *mesh_msgs_restore(mesh_conv_t conv, const mesh_msg_t *m);

// The outgoing message with this packet id, if still held.
mesh_msg_t *mesh_msgs_find_outgoing(uint32_t id);
// Move an outgoing message along. A status never goes backwards (a late
// "relayed" can't undo "delivered"), except that FAILED always wins over
// QUEUED/SENT/RELAYED.
void mesh_msgs_set_status(mesh_msg_t *m, mesh_msg_status_t status, uint8_t error);
