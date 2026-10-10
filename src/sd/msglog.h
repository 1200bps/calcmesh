// Message history on the SD card: every text message the store (mesh/msgs.h)
// sees is appended to a per-conversation file, and at boot the newest of them
// are loaded back, so chats survive a reboot.
//
// On the card:
//
//   /calcmesh/msgs/ch-LongFast-1c2d3e4f.tsv   channel LongFast
//   /calcmesh/msgs/dm-a1b2c3d4.tsv            DMs with node !a1b2c3d4
//   /calcmesh/msgs/channels.txt               which index is which channel
//
// A channel's file is named for the channel rather than its index, so its
// history goes with it when the node's channels are reordered: the name, with
// anything but letters, digits, - and _ made _, then a hash of the name and
// key (two channels can share a name). channels.txt keeps the node's channel
// table as last seen, so history is restored at boot before the node
// answers; msglog_sync_channels() brings it up to date once it does.
//
// Earlier versions named channel files by index (ch0.tsv). Those are read as
// that index until the node says which channel it is, then renamed.
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
#define MSGLOG_CHANNELS MSGLOG_DIR "/channels.txt"

// A file's name without directory or .tsv: "ch-" + 11 name bytes + "-" + 8.
#define MSGLOG_NAME_MAX 23
#define MSGLOG_PATH_MAX (sizeof MSGLOG_DIR + 1 + MSGLOG_NAME_MAX + 4)

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

// The node's channel table (mesh_db) is complete (handshake done): point
// each channel index at its channel's file, renaming old ch<index>.tsv files
// and reloading a conversation whose index now holds a different channel.
// Writes anything queued first. 1: something changed; 0: nothing did, or
// offline; -1: card error (we go offline).
int msglog_sync_channels(void);

// Write one queued record. 0: nothing queued; 1: wrote one; -1: card error
// (we go offline; reopen with msglog_open(false)).
int msglog_flush(void);
bool msglog_pending(void);

const msglog_stats_t *msglog_stats(void);

// ---- exposed for the tests -------------------------------------------------

// One record line, '\n'-terminated, into buf (MSGLOG_LINE_MAX + 1 bytes).
// *status_off is where the status column starts. Returns its length.
// The file name (MSGLOG_NAME_MAX + 1 bytes) for channel index's history, by
// mesh_db; false if that channel is disabled or not known well enough yet.
bool msglog_channel_file(int index, char *buf);
size_t msglog_format(const mesh_msg_t *m, uint32_t n, char *buf, size_t *status_off);
// Parse a record line (with or without the '\n'); modifies line. False if
// it isn't one (the header row, a torn write).
bool msglog_parse(char *line, mesh_msg_t *m, uint32_t *n, size_t *status_off);

// For get_fattime(): FatFs's packed date and time from epoch seconds (a fixed
// 2026-01-01 when the epoch is unknown).
uint32_t msglog_fattime(uint32_t epoch);
