#include "sd/sd.h"

#include <stdio.h>

#include "hardware/gpio.h"
#include "pico/time.h"

#include "ff.h"
#include "mesh/msgs.h"
#include "sd/msglog.h"

#define POLL_MS 100
#define DEBOUNCE 3          // polls the detect pin must hold still
#define RETRY_MS 10000      // after a card error

static FATFS fs;
static sd_state_t state = SD_NO_CARD;
static bool use_detect = true; // false if it said "empty" with a card in
static bool present;           // debounced detect
static int det_count;
static absolute_time_t next_poll, next_retry;
static bool error_reported;

static bool det_raw(void) { return !gpio_get(SD_DET_PIN); }

static const char *fs_name(void) {
    switch (fs.fs_type) {
    case FS_FAT12: return "FAT12";
    case FS_FAT16: return "FAT16";
    case FS_FAT32: return "FAT32";
    case FS_EXFAT: return "exFAT";
    default: return "?";
    }
}

static void unmount(void) {
    msglog_close();
    f_unmount("");
    sd_card_eject();
}

// Mount and start logging; with restore, load saved messages first.
static bool mount(bool restore) {
    FRESULT r = f_mount(&fs, "", 1);
    if (r == FR_OK) {
        int n = msglog_open(restore);
        if (n >= 0) {
            printf("[sd] %s card, %s", sd_card_type(), fs_name());
            if (restore)
                printf(", %d messages restored", n);
            printf("\n");
            state = SD_READY;
            error_reported = false;
            return true;
        }
    } else if (!error_reported) {
        printf("[sd] card won't mount (FatFs error %d%s)\n", r,
               r == FR_NO_FILESYSTEM ? ": not FAT or exFAT" : "");
    }
    unmount();
    error_reported = true;
    state = SD_ERROR;
    next_retry = make_timeout_time_ms(RETRY_MS);
    return false;
}

void sd_init(void) {
    sd_card_pins_init();
    gpio_init(SD_DET_PIN);
    gpio_set_dir(SD_DET_PIN, GPIO_IN);
    gpio_pull_up(SD_DET_PIN);
    busy_wait_us(100); // let the pull-up settle
    present = det_raw();
    det_count = 0;

    // Try even if detect says empty: if a card turns up anyway, that slot's
    // switch can't be trusted and we go by errors instead. (No complaint if
    // it fails with the slot empty.)
    error_reported = !present;
    if (mount(true)) {
        if (!present) {
            printf("[sd] card detect pin reads empty with a card in; ignoring it\n");
            use_detect = false;
        }
    } else if (!present) {
        state = SD_NO_CARD; // the expected failure: say nothing more
        error_reported = false;
    }
    next_poll = make_timeout_time_ms(POLL_MS);
}

void sd_task(void) {
    if (time_reached(next_poll)) {
        next_poll = make_timeout_time_ms(POLL_MS);
        if (use_detect) {
            bool now = det_raw();
            det_count = now == present ? 0 : det_count + 1;
            if (det_count >= DEBOUNCE) {
                present = now;
                det_count = 0;
                if (present) {
                    // Nothing in the store yet: as good as booting with it.
                    mount(mesh_msgs_conv_count() == 0);
                } else {
                    if (state == SD_READY)
                        printf("[sd] card removed\n");
                    unmount();
                    state = SD_NO_CARD;
                    error_reported = false;
                }
            }
        }
        if (state == SD_ERROR && (present || !use_detect) && time_reached(next_retry))
            mount(mesh_msgs_conv_count() == 0);
    }

    if (state == SD_READY && msglog_pending() && msglog_flush() < 0) {
        unmount();
        state = SD_ERROR;
        error_reported = true;
        next_retry = make_timeout_time_ms(RETRY_MS);
    }
}

sd_state_t sd_state(void) { return state; }

void sd_print_status(void) {
    static const char *const names[] = {
        [SD_NO_CARD] = "no card", [SD_READY] = "ready", [SD_ERROR] = "error"};
    const msglog_stats_t *s = msglog_stats();
    printf("sd: %s", names[state]);
    if (state == SD_READY)
        printf(", %s card, %s", sd_card_type(), fs_name());
    if (!use_detect)
        printf(" (card detect ignored)");
    printf("\nhistory: %lu restored, %lu written, %lu not saved, %lu errors\n",
           (unsigned long)s->restored, (unsigned long)s->written,
           (unsigned long)s->dropped, (unsigned long)s->errors);
}
