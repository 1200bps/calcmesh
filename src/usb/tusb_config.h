// TinyUSB device config: two CDC ports (console, node bridge) plus the SDK's
// reset interface. Replaces pico_stdio_usb's built-in config, which only has
// one CDC; see usb_descriptors.c for the layout.
#pragma once

// pico_stdio_usb's reset_interface.c gets its PICO_STDIO_USB_* defaults
// through this header, as with the SDK's own tusb_config.h.
#include "pico/stdio_usb.h"

#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE

#define CFG_TUD_CDC 2
#define CFG_TUD_CDC_RX_BUFSIZE 256
#define CFG_TUD_CDC_TX_BUFSIZE 1024 // node -> PC; the want_config dump is bursty
#define CFG_TUD_CDC_EP_BUFSIZE 64

// The reset interface has its own driver (pico_stdio_usb's reset_interface.c);
// the vendor class is only there to answer the Microsoft OS 2.0 descriptor
// request, same as the SDK's own config.
#define CFG_TUD_VENDOR 1
#define CFG_TUD_VENDOR_RX_BUFSIZE 256
#define CFG_TUD_VENDOR_TX_BUFSIZE 256
