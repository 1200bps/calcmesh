// A made-up but plausible mesh for rendering and testing the UI on the host.
#pragma once

#include <stdint.h>

#include "link.h"
#include "msgs.h"

#define SCENE_NOW 1791590400u // the wall clock the scene is set at
#define SCENE_ME 0xa1b2c3d4u

// What the stand-in link (fake_link.c) reports
extern mesh_link_state_t fake_link_state;
extern mesh_send_result_t fake_send_result; // what sending returns
extern uint32_t fake_epoch;                 // what mesh_link_epoch() returns

void scene_load(void);           // nodes, channels and messages
uint32_t scene_node(int i);      // node num of the i-th made-up node

// Store a message as the link would (incoming ones arrive unread)
const mesh_msg_t *scene_msg(uint32_t from, mesh_conv_t conv, uint32_t time,
                            uint8_t status, const char *text);
