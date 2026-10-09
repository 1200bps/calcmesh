// Message history on the SD card: every text message the store (mesh/msgs.h)
// sees is appended to a per-conversation file, and at boot the newest of them
// are loaded back, so chats survive a reboot.
//
// On the card:
//
//   /calcmesh/msgs/ch0.tsv           channel index 0 (one file per index)
//   /calcmesh/msgs/dm-a1b2c3d4.tsv   DMs with node !a1b2c3d4
//
// Each file is tab-separated text, one message per line, under a header row:
//
//   n  time  from  to  ch  id  reply  status  err  snr  rssi  hops  flags  text
//
//   n       record number, increasing across all files (orders conversations)
//   time    UTC by the node's clock, 2026-10-09T22:41:08Z, or - if unknown
//   from/to node numbers, !a1b2c3d4; to is !ffffffff for a channel message
//   ch      channel index
//   id      packet id, 8 hex digits; reply: the id this answers, or 00000000
//   status  received sending sent relayed delivered failed, space-padded to 9
//   err     Routing.Error number when failed, 3 digits
//   snr rssi hops   incoming only, else -; hops is - if unknown
//   flags   e = emoji reaction, m = came via MQTT, - = none
//   text    UTF-8, with \\ \t \n \r escaped
//
// so it opens in a spreadsheet or `column -ts$'\t'` as is. The status and err
// columns are fixed width because they're rewritten in place as an outgoing
// message is acked. Other files may live under /calcmesh/ later (logs,
// stats); this module only touches msgs/.
//
// Card I/O doesn't happen in the store's callbacks: records are queued in
// RAM and written by msglog_flush(), one per call, from the main loop.
// Portable: needs only FatFs (ff.h) and a mounted volume, so the host tests
// run it on a RAM disk.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mesh/msgs.h"

#define MSGLOG_ROOT "/calcmesh"
#define MSGLOG_DIR MSGLOG_ROOT "/msgs"

// Longest line we write or read: the fields, plus text escaped at worst 2x.
#define MSGLOG_LINE_MAX (120 + 2 * MESH_MSG_TEXT_MAX)

typedef struct {
    uint32_t restored; // messages loaded back at the last msglog_open()
    uint32_t written;  // records written (new messages and status changes)
    uint32_t dropped;  // not written: card offline, or the queue was full
    uint32_t errors;   // card errors while writing
} msglog_stats_t;

// The card is mounted: create the directories, find the next record number,
// and with restore, load the newest messages of each conversation into the
// store (an outgoing message still waiting for its ack becomes failed: timed
// out, as the link does when the node reboots). Then start logging. Returns
// messages restored, or -1 on a card error (still offline then).
int msglog_open(bool restore);
// The card is gone: stop writing. Queued records are dropped.
void msglog_close(void);
bool msglog_online(void);

// Write one queued record. 0: nothing queued; 1: wrote one; -1: card error
// (we go offline; reopen with msglog_open(false)).
int msglog_flush(void);
bool msglog_pending(void);

const msglog_stats_t *msglog_stats(void);

// ---- exposed for the tests -------------------------------------------------

// One record line, '\n'-terminated, into buf (MSGLOG_LINE_MAX + 1 bytes).
// *status_off is where the status column starts. Returns its length.
size_t msglog_format(const mesh_msg_t *m, uint32_t n, char *buf, size_t *status_off);
// Parse a record line (with or without the '\n'); modifies line. False if
// it isn't one (the header row, a torn write).
bool msglog_parse(char *line, mesh_msg_t *m, uint32_t *n, size_t *status_off);

// For get_fattime(): FatFs's packed date and time from epoch seconds (a fixed
// 2026-01-01 when the epoch is unknown).
uint32_t msglog_fattime(uint32_t epoch);
