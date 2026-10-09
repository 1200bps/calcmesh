// STAND-IN for src/mesh/msgs.h and the send half of link.h, which the "text
// messaging" work is building: their public declarations, copied, and
// implemented over a flat array (fake_msgs.c) so the UI can be rendered and
// tested before the real store lands. Delete this file and fake_msgs.c then.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MESH_MSG_POOL 64
#define MESH_MAX_DMS 16
#define MESH_MSG_TEXT_MAX 233
#define MESH_MSG_SEND_MAX 200
#define MESH_ADDR_BROADCAST 0xFFFFFFFFu

typedef struct {
    uint32_t peer;
    uint8_t channel; // channel index; meaningless for DMs
} mesh_conv_t;
#define MESH_CONV_CHANNEL(ch) ((mesh_conv_t){.peer = 0, .channel = (ch)})
#define MESH_CONV_DM(node) ((mesh_conv_t){.peer = (node), .channel = 0})
#define MESH_CONV_IS_DM(c) ((c).peer != 0)

typedef enum {
    MESH_MSG_RECEIVED, MESH_MSG_QUEUED, MESH_MSG_SENT,
    MESH_MSG_RELAYED, MESH_MSG_DELIVERED, MESH_MSG_FAILED,
} mesh_msg_status_t;

typedef struct {
    uint32_t id;       // packet id
    uint32_t from;     // sender node number (our own for outgoing)
    uint32_t to;       // node number or MESH_ADDR_BROADCAST
    uint32_t seq;      // store-wide insertion order
    uint32_t rx_time;  // epoch seconds, node clock; 0 if unknown
    uint32_t local_ms; // our clock when stored
    uint32_t reply_id; // 0 if none
    float snr;         // incoming only
    int16_t rssi;      // incoming only; 0 if not reported
    uint8_t hops;      // incoming only; 0xFF unknown
    uint8_t channel;
    uint8_t status;    // mesh_msg_status_t
    uint8_t error;     // meshtastic_Routing_Error when FAILED
    bool emoji;        // reaction to reply_id
    bool via_mqtt;
    uint8_t len;
    char text[MESH_MSG_TEXT_MAX + 1]; // UTF-8, NUL-terminated
} mesh_msg_t;

typedef struct {
    mesh_conv_t conv;
    uint16_t count;
    uint16_t unread;
    uint32_t last_seq;
} mesh_conv_info_t;

void mesh_msgs_clear(void);
uint32_t mesh_msgs_version(void);
int mesh_msgs_conv_count(void);
const mesh_conv_info_t *mesh_msgs_conv_at(int i);       // most recent first
const mesh_conv_info_t *mesh_msgs_conv(mesh_conv_t conv); // NULL if no messages
int mesh_msgs_count(mesh_conv_t conv);
const mesh_msg_t *mesh_msgs_recent(mesh_conv_t conv, int i); // 0 = newest
void mesh_msgs_mark_read(mesh_conv_t conv);
uint32_t mesh_msgs_unread_total(void);
mesh_conv_t mesh_msgs_conv_of(const mesh_msg_t *m);
const char *mesh_msgs_status_name(const mesh_msg_t *m);
mesh_msg_t *mesh_msgs_add(mesh_conv_t conv, const mesh_msg_t *m); // NULL on dup

// ---- from link.h ----
typedef enum {
    MESH_SEND_OK,
    MESH_SEND_NOT_READY,  // handshake not finished, or desktop mode has the node
    MESH_SEND_EMPTY,
    MESH_SEND_TOO_LONG,   // over MESH_MSG_SEND_MAX bytes
    MESH_SEND_NO_CHANNEL, // that channel index isn't enabled on the node
    MESH_SEND_FAILED,     // couldn't encode or store it
} mesh_send_result_t;
mesh_send_result_t mesh_link_send_text(mesh_conv_t conv, const char *text,
                                       size_t len, uint32_t reply_id,
                                       mesh_msg_t **out);
const char *mesh_link_send_result_name(mesh_send_result_t r);
uint32_t mesh_link_epoch(void); // node clock estimate, 0 until known

// ---- fake only ----
extern mesh_send_result_t fake_send_result; // what sending returns
extern uint32_t fake_epoch;                 // what mesh_link_epoch() returns
