// Turns the keyboard controller's FIFO entries into kbd_event_t. No hardware
// here, so the host tests can run it.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input/keys.h"

// FIFO entry states (key_state in the firmware's keyboard.h).
enum {
    KBD_STATE_IDLE = 0, // empty FIFO reads back as 0, 0
    KBD_STATE_PRESSED = 1,
    KBD_STATE_HOLD = 2,
    KBD_STATE_RELEASED = 3,
};

typedef struct {
    uint8_t mods;
    uint8_t down[8]; // raw codes currently held, 0 = free slot
} kbd_decoder_t;

// Feeds one FIFO entry. Returns true and fills *ev when it's a key-down or
// repeat worth reporting.
bool kbd_decode(kbd_decoder_t *d, uint8_t state, uint8_t code, kbd_event_t *ev);

// A short name for a key ("a", "Enter", "F3"), for logs.
const char *kbd_key_name(uint16_t key);
