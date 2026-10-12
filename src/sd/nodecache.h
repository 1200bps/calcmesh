// The node table (mesh/nodedb.h) kept on the SD card, so nodes heard in
// earlier sessions are still listed after a reboot, or after our node
// forgets them (its NodeDB is smaller, and a reflash or factory reset
// empties it). They come back as cached: our node knows nothing about them
// until they're heard again, so it can't send them DMs yet.
//
//   /calcmesh/nodes.tsv
//
//   # calcmesh nodes, heard through !a1b2c3d4
//   num  id  short  long  hw  role  user  heard  snr  hops  ch  mqtt
//   lat  lon  alt  pos_time  prec
//
// One line per node, tab-separated, under a header row: the node number in
// hex, then the fields of mesh_node_t, - for none. Only nodes heard through
// the same radio are restored. Our own node isn't kept; it's in every dump.
//
// Saved every NODECACHE_SAVE_MS while anything changed, a line per call to
// nodecache_flush() from the main loop (so a full table doesn't hold the
// loop up), into nodes.new, which then replaces nodes.tsv. Portable: needs
// only FatFs.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NODECACHE_PATH "/calcmesh/nodes.tsv"
#define NODECACHE_TMP "/calcmesh/nodes.new"
#ifndef NODECACHE_SAVE_MS
#define NODECACHE_SAVE_MS (10u * 60 * 1000)
#endif

typedef struct {
    uint32_t restored, saves, errors;
} nodecache_stats_t;

void nodecache_set_clock(uint32_t (*now_ms)(void));

// Add the saved nodes we don't know already. Number restored, or -1 on a
// card error (no file is 0).
int nodecache_load(void);

// A save is due or under way.
bool nodecache_pending(void);
// Write the next line of the save. 0 done, -1 card error (the save is
// dropped, and tried again next time).
int nodecache_flush(void);
// Stop a save under way (the card is going away).
void nodecache_close(void);

const nodecache_stats_t *nodecache_stats(void);
// Forget save state and stats (tests).
void nodecache_clear(void);
