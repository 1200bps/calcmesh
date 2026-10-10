// Another node's settings, read and written over the mesh with PKI admin
// messages: the remote node acts on them only if our node's public key is
// one of its admin keys (Settings > Security > Admin key in the phone app).
//
// Everything goes one message at a time, each waiting for its answer (gets)
// or its ack (sets), since mesh packets can overtake each other. Changes need
// the session key the remote node hands out with every answer, good for five
// minutes, so a write first asks for the owner again to get a fresh one.
//
// nodeedit.h uses this for the Settings tab when it's pointed at another
// node; it's separate only to keep the bookkeeping out of the way.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "nodeedit.h"

// Besides the MESH_EDIT_* sections: the device metadata (firmware version)
#define RA_META (1u << 7)

typedef enum {
    RA_IDLE,
    RA_BUSY,
    RA_DONE,   // the last fetch or write went through
    RA_FAILED, // ra_error() says why
} ra_state_t;

// Not routing errors: no answer at all, or no link to our own node
#define RA_ERR_NO_ANSWER 250
#define RA_ERR_NO_LINK 251

typedef struct {
    uint32_t node;
    mesh_edit_t s; // its settings, as last fetched
    uint32_t have; // sections in s (MESH_EDIT_* and RA_META)
    char firmware[18];
    uint32_t version; // bumped whenever s changes
} ra_remote_t;

extern ra_remote_t ra;

// Point at a node, forgetting what we had if it's a different one.
void ra_select(uint32_t node);
// Ask for these sections (MESH_EDIT_* | RA_META; channels stop at the first
// disabled one). False if busy or there's no link.
bool ra_fetch(uint32_t sections);
// Write these sections of e: fresh session key, begin_edit_settings, the
// sections, commit_edit_settings; then wait for the node to restart and read
// them back into s. False if busy or there's no link.
bool ra_write(const mesh_edit_t *e, uint32_t sections);
// Call regularly: timeouts and waits.
void ra_poll(void);

ra_state_t ra_state(void);
// Progress of the current or last job: steps done, of how many.
int ra_done(void);
int ra_total(void);
// Why it failed: a meshtastic_Routing_Error, or RA_ERR_*.
uint8_t ra_error(void);
// The current or last job was a write; and it has been sent, and we're
// waiting for the node or reading back.
bool ra_writing(void);
bool ra_written(void);

// One message's wait for its answer or ack, before giving up. The node
// itself retries an unacked message a few times first.
#define RA_STEP_MS 90000
// After the commit: the node restarts a few seconds later
#define RA_RESTART_MS 25000
