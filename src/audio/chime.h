// Message chimes on the PicoCalc's speakers: Morse "RR" for a direct message,
// "C" for a channel message, each switched on and off on its own in the
// settings (chime.dm, chime.channel; on unless set off).
//
// The speakers hang off GP26 (left) and GP27 (right), PWM slice 5 A and B
// (ClockworkPi's picocalc_helloworld pwm_sound.h). A chime is a square wave
// keyed by a hardware alarm, so the main loop's timing doesn't touch the
// rhythm.
#pragma once

#include <stdbool.h>

typedef enum { CHIME_DM, CHIME_CHANNEL, CHIME_KINDS } chime_kind_t;

void chime_init(void);
// Starts what's been asked for, and follows the settings.
void chime_task(void);
// Asks for a chime if that kind is switched on. One that is already playing
// or waiting absorbs another of the same kind, so a burst of messages gives
// one chime.
void chime_request(chime_kind_t kind);
// Same, whether or not it's switched on (console test).
void chime_play(chime_kind_t kind);
bool chime_enabled(chime_kind_t kind);
