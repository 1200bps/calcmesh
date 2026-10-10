#include "input/backlight.h"

#include <stdbool.h>
#include <stdint.h>

#include "pico/time.h"

#include "input/kbd.h"
#include "sd/settings.h"

// After writing a level, wait this long for the keyboard to report it back
// before trusting what it reports (it re-reads every 5 s; the keyboard may
// also round a hand-edited value, so it never matches).
#define SETTLE_MS 12000

typedef struct {
    const char *key;
    int (*get)(void);
    void (*set)(uint8_t);
    int seen;   // level last reported, -1 before the first report
    int want;   // level we last wrote, -1 if none pending
} light_t;

static light_t lights[] = {
    {SET_SCREEN_BACKLIGHT, kbd_lcd_backlight, kbd_set_lcd_backlight, -1, -1},
    {SET_KEYS_BACKLIGHT, kbd_key_backlight, kbd_set_key_backlight, -1, -1},
};
#define NLIGHTS (sizeof lights / sizeof *lights)

static absolute_time_t settle_until;
static uint32_t settings_seen;
static bool was_online;

// Put the saved levels back. force: the keyboard has just come back and its
// reports so far are stale.
static void restore(bool force) {
    bool wrote = false;
    for (size_t i = 0; i < NLIGHTS; i++) {
        light_t *l = &lights[i];
        int32_t v = settings_get_int(l->key, -1);
        if (v < 0 || v > 255 || (!force && v == l->get()))
            continue;
        l->set((uint8_t)v);
        l->want = (int)v;
        wrote = true;
    }
    if (wrote)
        settle_until = make_timeout_time_ms(SETTLE_MS);
    settings_seen = settings_version();
}

void backlight_init(void) {
    was_online = kbd_online();
    restore(true);
}

void backlight_task(void) {
    bool online = kbd_online();
    if (online && !was_online) {
        // The keyboard chip lost power (battery fault, cells swapped) and is
        // back on its defaults.
        for (size_t i = 0; i < NLIGHTS; i++)
            lights[i].seen = -1;
        restore(true);
    }
    was_online = online;

    // Settings loaded from a card that turned up later.
    if (settings_version() != settings_seen)
        restore(false);

    bool settling = !time_reached(settle_until);
    for (size_t i = 0; i < NLIGHTS; i++) {
        light_t *l = &lights[i];
        int now = l->get();
        if (now < 0)
            continue;
        if (l->want >= 0) {
            if (now != l->want && settling)
                continue; // our write hasn't been read back yet
            l->want = -1;
            l->seen = now;
            continue;
        }
        // A change made on the keyboard, the console or the UI. The first
        // report isn't a change: it's whatever the keyboard started with.
        if (l->seen >= 0 && now != l->seen)
            settings_set_int(l->key, now);
        l->seen = now;
    }
    // Our own saves bump the version too; nothing to restore for those.
    settings_seen = settings_version();
}
