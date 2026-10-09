#pragma once

#include <stdint.h>

// A blank RAM image of this many bytes for FatFs drive 0 (format it with
// f_mkfs).
void ramdisk_create(uint32_t bytes);
// Let `after` more sector reads/writes through, then fail them all; -1 heals.
void ramdisk_fail(long after);
// What get_fattime() reports, as epoch seconds.
extern uint32_t ramdisk_epoch;
