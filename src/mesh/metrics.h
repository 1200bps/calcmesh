// Telemetry (telemetry.proto): the latest device, environment and power
// metrics and local stats each node has sent us, with a short history of the
// numbers worth graphing, and requests for a fresh set from one node.
//
// Nodes broadcast device metrics now and then (every 30 min by default) and
// environment or power metrics if they have the sensors. Our own node also
// hands its serial client device metrics every minute and local stats every
// so often, without transmitting. Local stats from another node only come by
// asking it.
//
// Kept apart from mesh_db, so a resync with the node keeps the history; the
// table holds MESH_METRICS_NODES nodes and forgets the longest-silent one.
// link.c feeds it.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "meshtastic/telemetry.pb.h"

#ifndef MESH_METRICS_NODES
#define MESH_METRICS_NODES 16
#endif
#ifndef MESH_METRICS_HIST
#define MESH_METRICS_HIST 48
#endif
// A node adds a history point at most this often (node clock, seconds), so
// our own node's minute-by-minute metrics cover hours, not minutes.
#define MESH_METRICS_GAP_S 600

typedef enum {
    MET_DEVICE, // battery, channel and airtime use, uptime
    MET_ENV,    // temperature, humidity, pressure, light, wind...
    MET_POWER,  // INA-style voltage/current channels
    MET_LOCAL,  // the node's own packet counters (LocalStats)
    MET_KINDS
} met_kind_t;

#define MET_NONE_U8 0xFF
#define MET_NONE_TEMP INT16_MIN

// One history point. Each telemetry packet adds what it carried; the rest
// stays "none".
#define MET_NONE_U16 0xFFFF

typedef struct {
    uint32_t time;  // node clock, epoch seconds
    uint16_t mv;    // battery voltage, mV; 0 none
    uint8_t batt;   // percent, 101 external power; MET_NONE_U8 none
    uint8_t chutil; // channel utilisation, half-percent steps; MET_NONE_U8 none
    int16_t temp;   // tenths of a degree C; MET_NONE_TEMP none
    uint16_t airtx; // airtime used transmitting, hundredths of a percent; MET_NONE_U16 none
    // From local stats:
    uint16_t online; // nodes online; MET_NONE_U16 none
    uint16_t rxph;   // packets received per hour since the last local stats; MET_NONE_U16 none
    int8_t noise;    // noise floor, dBm; 0 none
} met_point_t;

typedef struct {
    uint32_t num;
    uint32_t time[MET_KINDS]; // when each kind last arrived (node clock); 0 never
    bool have[MET_KINDS];
    meshtastic_DeviceMetrics device;
    meshtastic_EnvironmentMetrics env;
    meshtastic_PowerMetrics power;
    meshtastic_LocalStats local;
    uint8_t nhist; // points in hist, oldest at hist_first
    uint8_t hist_first;
    met_point_t hist[MESH_METRICS_HIST];
} mesh_metrics_t;

void mesh_metrics_clear(void);

// A Telemetry from node num, received at time (node clock; 0 if unknown).
// Returns the node's entry, or NULL for a variant we don't keep.
mesh_metrics_t *mesh_metrics_add(uint32_t num, const meshtastic_Telemetry *t, uint32_t time);

// Device metrics carried in a NodeInfo (the want_config dump). Taken only if
// we have nothing newer; no history point.
void mesh_metrics_nodeinfo(uint32_t num, const meshtastic_DeviceMetrics *dm, uint32_t time);

// The node's entry, or NULL if nothing has come from it.
const mesh_metrics_t *mesh_metrics_find(uint32_t num);
int mesh_metrics_count(void);
const mesh_metrics_t *mesh_metrics_at(int i);

// History point i of e, oldest first.
const met_point_t *mesh_metrics_point(const mesh_metrics_t *e, int i);

// --- asking a node -------------------------------------------------------------

typedef enum {
    MET_REQ_IDLE,
    MET_REQ_WAITING,
    MET_REQ_DONE,   // it answered; the answer is in the table
    MET_REQ_FAILED, // mesh_metrics_req_error() says why
} met_req_state_t;

// Not routing errors: no answer at all, or no link to our own node
#define MET_ERR_NO_ANSWER 250
#define MET_ERR_NO_LINK 251

// How long to wait for an answer. Our own node answers over the serial link.
#define MET_REQ_REMOTE_MS 90000
#define MET_REQ_LOCAL_MS 10000

// Ask node for kind. To our own node it goes nowhere near the radio; to any
// other it's one packet out and one back. A new request replaces one still
// waiting (a late answer to the old one is still kept). False with no link.
bool mesh_metrics_request(uint32_t node, met_kind_t kind);
// The same, giving up after wait_ms instead.
bool mesh_metrics_request_wait(uint32_t node, met_kind_t kind, uint32_t wait_ms);
// Call regularly: gives up on a request after its time.
void mesh_metrics_poll(void);

met_req_state_t mesh_metrics_req_state(void);
uint32_t mesh_metrics_req_node(void);
met_kind_t mesh_metrics_req_kind(void);
// Milliseconds since the request went out (link clock)
uint32_t mesh_metrics_req_age_ms(void);
// meshtastic_Routing_Error or MET_ERR_*
uint8_t mesh_metrics_req_error(void);
// Goes up by one with every request, so whoever sent one can tell whether
// the state above is still about theirs.
uint32_t mesh_metrics_req_seq(void);

// From link.c: an answer or routing result for a packet we sent.
void mesh_metrics_answer(uint32_t request_id, uint32_t from);
void mesh_metrics_routing(uint32_t request_id, uint32_t from, uint8_t error);

const char *mesh_metrics_kind_name(met_kind_t k); // "device metrics", "local stats"...

// Called after each Telemetry is taken into the table (set once; NULL for
// none). Nothing on the card happens in it; metsched.c queues a log line.
typedef void (*met_add_cb)(const mesh_metrics_t *e, met_kind_t kind, uint32_t time);
void mesh_metrics_on_add(met_add_cb cb);

// Print one node's metrics (stdout; USB serial on the Pico).
void mesh_metrics_dump(uint32_t num);
