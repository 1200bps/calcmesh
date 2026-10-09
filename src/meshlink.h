// Link to the Meshtastic node: protobuf API over uart0 (GP0 TX, GP1 RX).
//
// Wire framing, per the Meshtastic serial API:
//   0x94 0xC3 <len hi> <len lo> <len bytes of protobuf>
// ToRadio goes out, FromRadio comes back. Anything that arrives outside a
// frame is the node's plain-text debug log.
#pragma once

#include <stdbool.h>

#include "meshtastic/mesh.pb.h"

#define MESHLINK_UART      uart0
#define MESHLINK_TX_PIN    0
#define MESHLINK_RX_PIN    1
#define MESHLINK_BAUD      115200

void meshlink_init(void);

// Wakes a sleeping node and resyncs its frame parser.
void meshlink_wake(void);

bool meshlink_send(const meshtastic_ToRadio *msg);

// Non-blocking. Drains the UART RX FIFO; returns true and fills *out when a
// complete FromRadio has been decoded. Out-of-frame bytes go to `log_cb` if
// set.
bool meshlink_poll(meshtastic_FromRadio *out);

typedef void (*meshlink_log_cb)(char c);
void meshlink_set_log_cb(meshlink_log_cb cb);
