// History for the Metrics tab's graphs, read back from the metrics log on the
// SD card (metlog.h), so a reboot doesn't start the graphs over: one node,
// over a window of time (METVIEW_SPANS: 3 hours to a week), cut into
// METVIEW_BINS slots, each the mean of the log lines that fell in it.
//
// The log only grows, so each file is searched for where the window starts
// (its lines are in time order, near enough) and read on from there, a
// little per metview_task() call from the main loop, so the node's serial
// link never waits long on the card. Until a load is done, metview_ready()
// is false and the graphs make do with the history in RAM (metrics.h).
//
// Telemetry newer than the last line read is the caller's to add from RAM:
// metview_newest() says where the card left off. Lines still queued for the
// card are in RAM too.
//
// Portable: needs only FatFs.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MV_MV,     // battery, volts
    MV_BATT,   // battery, percent (not "powered")
    MV_CHUTIL, // channel utilisation, percent
    MV_TEMP,   // temperature, °C
    MV_AIRTX,  // airtime used transmitting, percent
    MV_NOISE,  // noise floor, dBm
    MV_ONLINE, // nodes online
    MV_RXPH,   // packets received per hour
    MV_SERIES
} mv_series_t;

#ifndef METVIEW_BINS
#define METVIEW_BINS 128
#endif

// The windows [ and ] step through, seconds
#define METVIEW_SPANS {3 * 3600, 12 * 3600, 24 * 3600, 3 * 86400, 7 * 86400}
#define METVIEW_NSPANS 5

// Load node's history for the span seconds up to end (node clock), unless
// it's loaded or loading. For the same node and span, a later end reloads
// only once it has moved on a quarter of the span (at most 4 hours): the
// RAM history covers the gap meanwhile.
void metview_want(uint32_t node, uint32_t span, uint32_t end);

// Whether what metview_want() last asked for is loaded
bool metview_ready(void);
uint32_t metview_node(void);
uint32_t metview_start(void); // the window: start to start + span
uint32_t metview_span(void);
// The newest line read from the card; 0 none
uint32_t metview_newest(void);
// Slot i's mean of series s, NAN if nothing fell in it; its time is the
// middle of the slot.
float metview_value(int i, mv_series_t s);
uint32_t metview_time(int i);

// For sd.c: a card is mounted or not. Loads wait for one (with no card a
// load is done at once, empty), and start over when it comes back.
void metview_set_online(bool online);
bool metview_pending(void);
// Read some more. 0 fine, -1 card error.
int metview_task(void);
// Forget everything (tests).
void metview_clear(void);
