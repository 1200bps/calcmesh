// Metrics log on the SD card: the telemetry we get from every node (see
// metsched.h for what's left out), a file per node and kind, read back for
// the graphs by metview.h:
//
//   /calcmesh/metrics/a1b2c3d4-device.tsv
//   /calcmesh/metrics/a1b2c3d4-local.tsv     (and -env, -power)
//
// Tab-separated, under a header row, - for a value the node didn't send:
//
//   time  got  battery  volts  chutil  airtx  uptime                (device)
//   time  got  temp  humidity  pressure  gas  iaq  lux  wind  wind_dir
//         gust  rain_1h  rain_24h  volts  current                   (env)
//   time  got  ch1_v  ch1_ma  ch2_v  ch2_ma  ch3_v  ch3_ma          (power)
//   time  got  uptime  chutil  airtx  tx  rx  rx_bad  rx_dupe  relayed
//         relay_cancel  tx_dropped  online  nodes  noise  heap_free  (local)
//
//   time  UTC by the node's clock, 2026-10-11T15:30:00Z, or -
//   got   answer (to our request), heard (sent unasked), or for a kind a
//         scheduled run got nothing for: no-answer, or the Routing.Error
//         number as error-8 (the rest of the line is -)
//
// Lines are queued in RAM and written by metlog_flush(), one per call, from
// the main loop, like msglog.h. Portable: needs only FatFs.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mesh/metrics.h"

#define METLOG_DIR "/calcmesh/metrics"
#define METLOG_LINE_MAX 200

typedef struct {
    uint32_t written, dropped, errors;
} metlog_stats_t;

void metlog_telemetry(const mesh_metrics_t *e, met_kind_t kind, uint32_t time, bool asked);
// error: meshtastic_Routing_Error or MET_ERR_*
void metlog_failed(uint32_t node, met_kind_t kind, uint8_t error, uint32_t time);

// For sd.c: a card is mounted (lines are written) or not (they wait, until
// the queue is full; then the newest are dropped).
void metlog_set_online(bool online);
bool metlog_pending(void);
// Write the oldest queued line. 0 done, -1 card error (it stays queued).
int metlog_flush(void);
const metlog_stats_t *metlog_stats(void);
// Forget the queue and stats (tests).
void metlog_clear(void);
