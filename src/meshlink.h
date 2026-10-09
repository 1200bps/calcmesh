// uart0 transport for the Meshtastic serial API (GP0 TX, GP1 RX, 115200 8N1).
// Framing and the protocol itself live in src/mesh/; this only moves bytes.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define MESHLINK_UART      uart0
#define MESHLINK_TX_PIN    0
#define MESHLINK_RX_PIN    1
#define MESHLINK_BAUD      115200

// Sets up the UART with an interrupt-driven receive buffer and starts the
// want_config handshake.
void meshlink_init(void);

// Call from the main loop: feeds received bytes to the protocol and runs its
// timers.
void meshlink_task(void);

// Bytes lost because the receive buffer was full (main loop too slow).
uint32_t meshlink_rx_overruns(void);
