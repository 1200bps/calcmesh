// Screen mirror on a third USB serial port ("calcmesh screen"), standing in
// for the LCD: a PC reads the framebuffer from it and draws it
// (tools/screen.py). Nothing is sent while no program has the port open, and
// sending never blocks the main loop: changed rows are queued and go out as
// the port's transmit buffer drains, read from the framebuffer at that time.
//
// Stream format: packets of a 4-byte header, 0xFB, a type byte, and a
// little-endian 16-bit argument, then a payload.
//
//   'P' 0      palette: 4 x RGB888 (12 bytes), shades 0..3 (fb_palette)
//   'R' y      framebuffer row y: FB_STRIDE (80) bytes, 2 bits per pixel,
//              leftmost pixel in the top bits
//   'E' 0      end of frame: every row sent so far is current; no payload
//
// On open the PC gets the palette and then every row.
//
// The PC can send keys back the same way, so the PicoCalc can be driven from
// the PC's keyboard (tools/screen.py -v):
//
//   'K' key    a keypress: key is a KBD_* code (input/keys.h), then 1 byte,
//              KBD_MOD_* bits, plus 0x80 if it's an auto-repeat
//
// Anything else from the PC is skipped.
#pragma once

#include <stdbool.h>

#include "input/keys.h"

// USB CDC instance of the screen port (see usb/usb_descriptors.c).
#define SCREENPORT_USB_CDC 2

// An fb_flush() sink: queue rows y0..y1 (inclusive).
void screenport_rows(int y0, int y1, void *ctx);

// Call from the main loop: sends what fits.
void screenport_task(void);

// Whether a program has the port open.
bool screenport_open(void);

// The next key the PC sent, if any.
bool screenport_key(kbd_event_t *ev);
