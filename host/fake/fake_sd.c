// Host-only stand-in for sd.c's card state.
#include <stdint.h>

#include "sd/sd.h"

int fake_sd_state = SD_READY;

sd_state_t sd_state(void) { return (sd_state_t)fake_sd_state; }

// For ramdisk.c's get_fattime(); msglog.c isn't in the UI build.
uint32_t msglog_fattime(uint32_t epoch);
uint32_t msglog_fattime(uint32_t epoch) {
    (void)epoch;
    return (uint32_t)(2026 - 1980) << 25 | 1u << 21 | 1u << 16;
}
