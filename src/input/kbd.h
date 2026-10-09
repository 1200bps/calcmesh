// PicoCalc keyboard, read from its STM32 controller over I2C.
//
// The STM32 (ClockworkPi's picocalc_keyboard firmware) scans the matrix,
// applies Shift and Caps Lock itself and queues key events in a FIFO we poll
// at register 0x09. It sits on i2c1, GP6 = SDA, GP7 = SCL, address 0x1F
// (clockwork_Mainboard_V2.0_Schematic.pdf; conf_app.h in the firmware).
//
// The STM32 also masters a second bus to the power chip, and ClockworkPi's
// own Pico code runs this one at 10 kHz; a register read is ~5 ms of bus
// time at that speed, so transfers run in the background off kbd_task().
#pragma once

#include <stdbool.h>

#include "input/keys.h"

#define KBD_I2C      i2c1
#define KBD_SDA_PIN  6
#define KBD_SCL_PIN  7
#define KBD_ADDR     0x1F
#define KBD_BAUD     10000

// Key codes and kbd_event_t are in keys.h. Where the keyboard puts them:
// Home = Shift+Tab, End = Shift+Del, PgUp/PgDn = Shift+Up/Down,
// Insert = Shift+Enter or Alt+I, Break = Shift+Esc, F6..F10 = Shift+F1..F5.
// Auto-repeat starts after the key is held 300 ms.

// Sets up the I2C pins and starts polling.
void kbd_init(void);

// Call from the main loop: drives the I2C transfers and queues key events.
void kbd_task(void);

// Takes the next key-down or repeat, oldest first. Releases and modifier
// keys on their own aren't reported. With Ctrl or Alt held, letters come
// through lower case (Ctrl+C is key 'c', mods KBD_MOD_CTRL; Shift shows up
// in mods).
bool kbd_get(kbd_event_t *ev);

// Whether the keyboard answered its last poll.
bool kbd_online(void);

// A short name for a key ("a", "Enter", "F3"), for logs.
const char *kbd_key_name(uint16_t key);
