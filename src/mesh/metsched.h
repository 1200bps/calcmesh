// Scheduled metrics: ask a node for some kinds of telemetry (metrics.h)
// every so often, for the graphs and a log on the SD card.
//
// Each schedule names a node, which kinds to ask for and how often, no
// oftener than every METSCHED_MIN_MIN minutes. A run asks for its kinds one
// at a time, a minute (METSCHED_GAP_MS) after the last one was answered or
// given up on: LongFast is slow, and an answer from several hops away can
// take a while. One that gets no answer within METSCHED_WAIT_MS is asked
// once more METSCHED_RETRY_MS later (the other kinds go meanwhile), then
// counted as failed until the next run. A request the user makes by hand
// meanwhile goes first; the run carries on after it. Runs wait while the
// link is down; after boot the first comes METSCHED_FIRST_MS after the link
// is up, a new schedule's at once.
//
// Kept in the settings (settings.h) as metrics.sched1..4:
//
//   metrics.sched1 = !a1b2c3d4 dl 60
//
// node, kinds (d device, e environment, p power, l local stats) and minutes;
// an empty value is a free slot.
//
// Telemetry goes to the SD card log (metlog.h): every answer to a request,
// and what nodes send unasked (our own node's minute-by-minute reports one
// per ten minutes), plus a line for each kind a run got nothing for.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "metrics.h"

#define METSCHED_MAX 4
#define METSCHED_MIN_MIN 30
#define METSCHED_MAX_MIN 1440
#define METSCHED_WAIT_MS 90000   // for another node's answer (ours: MET_REQ_LOCAL_MS)
#define METSCHED_GAP_MS 60000    // between one request's end and the next
#define METSCHED_RETRY_MS 300000 // after no answer, before asking once more
#define METSCHED_FIRST_MS 120000 // after the link first comes up

#define MET_BIT(k) (1u << (k))

typedef struct {
    uint32_t node;
    uint8_t kinds; // MET_BIT()s
    uint16_t every_min;
    // Not saved:
    bool placed;          // next_ms is set
    uint32_t next_ms;     // link clock
    uint32_t started_ms;  // of the last run
    bool ran;             // started_ms means something
    uint32_t last_epoch;  // when the last run ended, node clock; 0 none yet
    uint8_t last_ok;      // kinds that run got
    uint8_t last_failed;  // kinds it didn't
    uint8_t last_error[MET_KINDS]; // why not (mesh_metrics_req_error())
} met_sched_t;

// Load from the settings and start logging telemetry. Once, at start.
void metsched_init(void);
// Call every pass: picks up changed settings, sends what's due.
void metsched_task(void);

int metsched_count(void);
const met_sched_t *metsched_at(int i);
// NULL if the node has no schedule.
const met_sched_t *metsched_find(uint32_t node);

// Make, change or (kinds 0) remove the node's schedule, and save it. every
// is held to METSCHED_MIN_MIN..METSCHED_MAX_MIN. A new one runs at once; a
// changed one when its new interval since the last run is up. False if all
// METSCHED_MAX are in use.
bool metsched_set(uint32_t node, uint8_t kinds, uint16_t every_min);

// The node's run in progress: true, with the kind it's asking for and the
// try (1 or 2). *kind is MET_KINDS between requests.
bool metsched_running(uint32_t node, met_kind_t *kind, int *try_no);
// Milliseconds until its next run; 0 when due or running.
uint32_t metsched_due_in_ms(const met_sched_t *s);

// The intervals offered in the UI, and the one before or after every.
uint16_t metsched_step(uint16_t every_min, int dir);

// "dl" for device and local stats
const char *metsched_kinds_str(uint8_t kinds, char buf[MET_KINDS + 1]);
// Parse one back; false on anything but d, e, p and l.
bool metsched_parse_kinds(const char *s, uint8_t *kinds);

// Console listing.
void metsched_print(void);
