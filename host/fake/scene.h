// A made-up but plausible mesh for rendering and testing the UI on the host.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "link.h"
#include "msgs.h"
#include "nodeedit.h"

#define SCENE_NOW 1791590400u // the wall clock the scene is set at
#define SCENE_ME 0xa1b2c3d4u

// What the stand-in link (fake_link.c) reports
extern mesh_link_state_t fake_link_state;
extern bool fake_node_silent; // in WAKING/CONFIGURING: report no node
extern mesh_send_result_t fake_send_result; // what sending returns
extern uint32_t fake_epoch;                 // what mesh_link_epoch() returns
extern mesh_link_stats_t fake_link_stats;   // what mesh_link_stats() returns
extern uint32_t fake_ms;                    // mesh_link_ms()
extern int fake_admin_sent;                 // admin messages "sent"
extern int fake_admin_last;                 // and the last one's payload tag
// Remote admin: the fake remote node's settings, a routing error it answers
// with instead (0: none), and messages sent to it. fake_remote_answer()
// answers the one waiting; false if none is.
extern mesh_edit_t fake_remote;
extern int fake_remote_error;
extern int fake_remote_sent;
bool fake_remote_answer(void);
// Telemetry requests: the one waiting (id 0: none), and how many were sent.
// scene_telemetry_answer() has the fake mesh answer it with made-up numbers,
// or with a routing error.
extern uint32_t fake_tel_to, fake_tel_id;
extern pb_size_t fake_tel_variant;
extern int fake_tel_sent;
bool scene_telemetry_answer(int error);
extern int fake_sd_state;                   // sd_state(): sd_state_t

void scene_load(void);           // nodes, channels and messages
uint32_t scene_node(int i);      // node num of the i-th made-up node

// Store a message as the link would (incoming ones arrive unread)
const mesh_msg_t *scene_msg(uint32_t from, mesh_conv_t conv, uint32_t time,
                            uint8_t status, const char *text);
