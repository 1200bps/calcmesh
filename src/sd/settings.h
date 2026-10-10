// Client settings: a small key/value table held in RAM and kept on the SD
// card as /calcmesh/settings.txt, so they survive a power cycle.
//
// The file is plain text, one setting per line:
//
//   # calcmesh settings
//   screen.backlight = 128
//   keys.backlight = 0
//
// Keys are dotted lowercase names; values are text (numbers as decimal).
// Blank lines and lines starting with # are skipped; keys we don't know are
// kept as they are when the file is rewritten. A rewrite goes to
// settings.new first and is then renamed over settings.txt, so pulling the
// power mid-save leaves one or the other whole.
//
// Without a card the table still works, in RAM; changes made then are saved
// once a card is mounted. Loading from a card never undoes a change made
// since boot: a key set before the load keeps its value (and gets saved).
//
// Portable: needs only FatFs (ff.h) and a mounted volume. sd.c loads it on
// mount and saves from sd_task(); everything else just gets and sets.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SETTINGS_PATH "/calcmesh/settings.txt"
#define SETTINGS_TMP "/calcmesh/settings.new"

#define SETTINGS_MAX 32       // keys held
#define SETTINGS_KEY_MAX 31   // bytes in a key
#define SETTINGS_VALUE_MAX 63 // bytes in a value

// Settings in use (add new ones here, so the file's keys stay in one place):
#define SET_SCREEN_BACKLIGHT "screen.backlight" // 0-255, as the keyboard reports it
#define SET_KEYS_BACKLIGHT "keys.backlight"     // 0-255, as the keyboard reports it
#define SET_DESKTOP_MODE "desktop.mode"         // auto, on or off (meshlink.h)
#define SET_DESKTOP_IDLE "desktop.idle_min"     // auto mode's idle timeout, 6-120
#define SET_KEY_ECHO "console.key_echo"         // print keypresses on the console
#define SET_CHIME_DM "chime.dm"                 // Morse RR for a DM (on unless off)
#define SET_CHIME_CHANNEL "chime.channel"       // Morse C for a channel message

// NULL if unset.
const char *settings_get(const char *key);
// def if unset or not a whole number.
int32_t settings_get_int(const char *key, int32_t def);
// "1"/"0", "on"/"off", "yes"/"no", "true"/"false"; def otherwise.
bool settings_get_bool(const char *key, bool def);

// False if the key or value is too long or has characters a line can't hold
// (newline, '=' or spaces in a key), or the table is full. Setting a key to
// the value it already has changes nothing.
bool settings_set(const char *key, const char *value);
bool settings_set_int(const char *key, int32_t value);
bool settings_set_bool(const char *key, bool value);

// Bumped by every change and every load, so callers can poll it and re-read.
uint32_t settings_version(void);

// ---- for sd.c ---------------------------------------------------------------

// Read the card's file into the table. FR_OK (0) also when there is no file
// yet; otherwise the FatFs error.
int settings_load(void);
// Changed since the last save (or since boot, when nothing's been saved).
bool settings_dirty(void);
// Milliseconds-clock time of the last change, for saving a little later.
uint32_t settings_changed_at(void);
void settings_set_clock(uint32_t (*now_ms)(void));
// Write the table to the card if it's dirty. FR_OK, or the FatFs error
// (it stays dirty then).
int settings_save(void);

// Forget everything (tests).
void settings_clear(void);
