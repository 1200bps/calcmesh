// uart0 transport for the Meshtastic serial API (GP0 TX, GP1 RX, 115200 8N1).
// Framing and the protocol itself live in src/mesh/; this only moves bytes.
//
// Desktop mode: while it's on, the second USB serial port ("calcmesh node")
// is a straight pipe to the node, so the meshtastic CLI or web client on a PC
// can use it as if the node were plugged in. Our own link goes passive (it
// stops sending but still decodes the node's replies) and runs a fresh
// handshake when desktop mode ends.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MESHLINK_UART      uart0
#define MESHLINK_TX_PIN    0
#define MESHLINK_RX_PIN    1
#define MESHLINK_BAUD      115200

// USB CDC instance of the node port (see usb/usb_descriptors.c).
#define MESHLINK_USB_CDC   1

typedef enum {
    MESHLINK_DESKTOP_AUTO, // on while a PC is using the node port (see meshlink.c)
    MESHLINK_DESKTOP_ON,   // always on
    MESHLINK_DESKTOP_OFF,  // never; the node port is ignored
} meshlink_desktop_t;

// Sets up the UART with an interrupt-driven receive buffer and starts the
// want_config handshake.
void meshlink_init(void);

// Call from the main loop: feeds received bytes to the protocol and runs its
// timers.
void meshlink_task(void);

void meshlink_set_desktop(meshlink_desktop_t mode);
meshlink_desktop_t meshlink_desktop(void);
// Auto mode ends a session after this long without a byte from the PC
// (default 10). Clients heartbeat every 5 minutes, so keep it above that.
void meshlink_set_desktop_idle_min(unsigned minutes);
// Whether the bridge is active right now.
bool meshlink_bridged(void);

// Bytes lost because the receive buffer was full (main loop too slow).
uint32_t meshlink_rx_overruns(void);
