// Traceroute (TRACEROUTE_APP, RouteDiscovery in mesh.proto): ask a node for
// the route our packets take to it and the route its answer takes back,
// with the SNR each hop was heard at.
//
// The request is one packet to the node with want_response and an empty
// RouteDiscovery. Each node that relays it adds itself to route and the SNR
// it heard it at to snr_towards; the destination adds its own SNR and
// answers. Relays on the way back fill route_back and snr_back, and our node
// adds the SNR it heard the answer at. A relay that didn't add itself (older
// firmware) shows up as MESH_TRACE_UNKNOWN with SNR MESH_TRACE_SNR_UNKNOWN.
//
// Firmware 2.7 takes at most one traceroute every 30 s from its clients and
// drops the rest with a client notification, so we hold back ourselves.
//
// Keeps the latest result for each of the last MESH_TRACES nodes traced; one
// request is in flight at a time. link.c feeds it.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MESH_TRACE_HOPS 8 // relays each way (max_count in mesh.options)
#define MESH_TRACES 8
#define MESH_TRACE_GAP_MS 30000u
// Wait this long per hop the request may take, plus one for the answer
#define MESH_TRACE_HOP_MS 20000u

#define MESH_TRACE_UNKNOWN 0xFFFFFFFFu // a relay that didn't say who it was
#define MESH_TRACE_SNR_UNKNOWN (-128)

typedef enum {
    TRACE_WAITING,
    TRACE_DONE,
    TRACE_FAILED, // error says why
} mesh_trace_state_t;

// Not routing errors
#define TRACE_ERR_NO_ANSWER 250
#define TRACE_ERR_REFUSED 252 // our node wouldn't send it; note has its words

typedef struct {
    uint32_t node;
    uint32_t id;      // packet id of the request
    uint32_t sent_ms; // mesh_link_ms() clock
    uint32_t wait_ms;
    uint32_t took_ms; // request to answer
    uint32_t epoch;   // node clock when sent; 0 if unknown
    uint8_t state;    // mesh_trace_state_t
    uint8_t error;    // meshtastic_Routing_Error or TRACE_ERR_*
    uint8_t n_out, n_back; // relays each way
    bool back_known; // the answer carried the way back (firmware 2.5 on)
    uint32_t out[MESH_TRACE_HOPS];
    int8_t snr_out[MESH_TRACE_HOPS + 1]; // dB * 4, at out[i]; the last at the node
    uint32_t back[MESH_TRACE_HOPS];
    int8_t snr_back[MESH_TRACE_HOPS + 1]; // at back[i]; the last at our node
    char note[48];
} mesh_trace_t;

typedef enum {
    TRACE_SENT,
    TRACE_NO_LINK,  // not connected to our node
    TRACE_TOO_SOON, // mesh_trace_wait_ms() says how long to go
    TRACE_BAD_NODE, // our own node, broadcast, or 0
} mesh_trace_send_t;

void mesh_trace_clear(void);

// Trace node. Transmits.
mesh_trace_send_t mesh_trace_send(uint32_t node);
// Milliseconds until another traceroute may go out; 0 now.
uint32_t mesh_trace_wait_ms(void);

// The latest trace to node, or NULL. Gives up on one that's had its time.
const mesh_trace_t *mesh_trace_find(uint32_t node);
// The one sent most recently, or NULL.
const mesh_trace_t *mesh_trace_latest(void);

// From link.c: an answer (a RouteDiscovery with request_id set), a routing
// result for a packet we sent, a notification from our node about one.
struct _meshtastic_RouteDiscovery;
void mesh_trace_answer(uint32_t request_id, uint32_t from,
                       const struct _meshtastic_RouteDiscovery *rd);
void mesh_trace_routing(uint32_t request_id, uint32_t from, uint8_t error);
void mesh_trace_notice(uint32_t reply_id, const char *text);

// "12Tc > ABCD (6.25) > 12Bi (-3.0)", one line per direction; stdout.
void mesh_trace_print(const mesh_trace_t *t);
