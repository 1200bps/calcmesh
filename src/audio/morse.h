// Morse timing: text to a list of key-down/key-up durations, for the message
// chimes (chime.h). Portable; the host tests check it.
#pragma once

#include <stdint.h>

// Element lengths in ms at wpm words per minute (PARIS: a dot is 1200/wpm
// ms). Letters A-Z and digits; a space is a word gap; anything else is
// skipped. out[0] is key-down, then they alternate (up, down, ...), ending
// on a key-down: the gap after the last element is the caller's. Returns
// how many were written (at most max), 0 if there's nothing to send.
int morse_timing(const char *text, int wpm, uint16_t *out, int max);
