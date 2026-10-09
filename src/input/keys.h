// Key codes and events from the PicoCalc keyboard, as the UI sees them.
// No hardware dependencies, so host builds can use it too.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Printable ASCII 0x20..0x7E as-is (the keyboard MCU applies shift and caps
// lock). Everything else:
enum {
    KBD_NONE = 0x00,
    KBD_BACKSPACE = 0x08,
    KBD_TAB = 0x09,
    KBD_ENTER = 0x0A,
    KBD_ESC = 0x1B,

    KBD_UP = 0x100,
    KBD_DOWN,
    KBD_LEFT,
    KBD_RIGHT,
    KBD_HOME,
    KBD_END,
    KBD_PGUP,
    KBD_PGDN,
    KBD_INSERT,
    KBD_DEL,
    KBD_BREAK,

    KBD_F1 = 0x110, // F1..F10 are contiguous
    KBD_F10 = KBD_F1 + 9,
};

// Modifiers held when the key went down.
#define KBD_MOD_SHIFT 0x01
#define KBD_MOD_CTRL 0x02
#define KBD_MOD_ALT 0x04

typedef struct {
    uint16_t key;
    uint8_t mods;
    bool repeat; // auto-repeat rather than a fresh press
} kbd_event_t;
