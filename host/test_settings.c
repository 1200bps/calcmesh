// Client settings (src/sd/settings.c) on a FAT-formatted RAM disk.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "ramdisk.h"
#include "sd/settings.h"

static int failures;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
            failures++;                                                        \
        }                                                                      \
    } while (0)

// ramdisk.c's get_fattime() wants it; msglog.c isn't in this test.
uint32_t msglog_fattime(uint32_t epoch);
uint32_t msglog_fattime(uint32_t epoch) {
    (void)epoch;
    return (uint32_t)(2026 - 1980) << 25 | 1u << 21 | 1u << 16;
}

static FATFS fs;
static uint32_t clock_now;
static uint32_t fake_clock(void) { return clock_now; }

static void fresh_card(void) {
    static BYTE work[FF_MAX_SS];
    ramdisk_create(4u << 20);
    MKFS_PARM opt = {.fmt = FM_FAT | FM_SFD};
    CHECK(f_mkfs("", &opt, work, sizeof work) == FR_OK);
    CHECK(f_mount(&fs, "", 1) == FR_OK);
}

static void write_file(const char *path, const char *text) {
    FIL f;
    UINT put;
    f_mkdir("/calcmesh");
    CHECK(f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK);
    CHECK(f_write(&f, text, (UINT)strlen(text), &put) == FR_OK);
    f_close(&f);
}

static char *slurp(const char *path) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK)
        return NULL;
    UINT n = (UINT)f_size(&f), got;
    char *buf = malloc(n + 1);
    CHECK(f_read(&f, buf, n, &got) == FR_OK && got == n);
    buf[got] = '\0';
    f_close(&f);
    return buf;
}

// Power cycle: RAM forgets, the card's file comes back.
static void reboot(void) {
    settings_clear();
    CHECK(f_mount(NULL, "", 0) == FR_OK);
    CHECK(f_mount(&fs, "", 1) == FR_OK);
    CHECK(settings_load() == FR_OK);
}

static void test_get_set(void) {
    settings_clear();
    CHECK(settings_get("a.b") == NULL);
    CHECK(settings_get_int("a.b", -1) == -1);
    CHECK(!settings_dirty());
    uint32_t v = settings_version();
    clock_now = 500;
    CHECK(settings_set_int("screen.backlight", 128));
    CHECK(settings_version() != v && settings_dirty() && settings_changed_at() == 500);
    CHECK(settings_get_int("screen.backlight", -1) == 128);
    v = settings_version();
    CHECK(settings_set("screen.backlight", "128")); // same: no change
    CHECK(settings_version() == v);
    CHECK(settings_set("ui.name", "Mara's calc = fun"));
    CHECK(strcmp(settings_get("ui.name"), "Mara's calc = fun") == 0);
    CHECK(settings_get_int("ui.name", 7) == 7);
    CHECK(settings_set_bool("ui.beep", true) && settings_get_bool("ui.beep", false));
    CHECK(settings_set("ui.beep", "Off") && !settings_get_bool("ui.beep", true));
    CHECK(settings_set("ui.beep", "maybe") && settings_get_bool("ui.beep", true));

    CHECK(!settings_set("", "x"));
    CHECK(!settings_set("has space", "x"));
    CHECK(!settings_set("a=b", "x"));
    CHECK(!settings_set("#x", "x"));
    CHECK(!settings_set("k", "two\nlines"));
    char big[SETTINGS_VALUE_MAX + 2];
    memset(big, 'v', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    CHECK(!settings_set("k", big));
    big[SETTINGS_VALUE_MAX] = '\0';
    CHECK(settings_set("k", big));

    settings_clear();
    char key[8];
    for (int i = 0; i < SETTINGS_MAX; i++) {
        snprintf(key, sizeof key, "k%d", i);
        CHECK(settings_set_int(key, i));
    }
    CHECK(!settings_set("one.more", "x"));
    CHECK(settings_set_int("k3", 33)); // existing keys still change
}

static void test_card(void) {
    fresh_card();
    settings_clear();
    CHECK(settings_load() == FR_OK); // no file yet
    CHECK(settings_save() == FR_OK && slurp(SETTINGS_PATH) == NULL); // not dirty

    settings_set_int(SET_SCREEN_BACKLIGHT, 96);
    settings_set_int(SET_KEYS_BACKLIGHT, 0);
    CHECK(settings_save() == FR_OK && !settings_dirty());
    char *f = slurp(SETTINGS_PATH);
    CHECK(f && strcmp(f, "# calcmesh settings: key = value, one per line\n"
                         "screen.backlight = 96\nkeys.backlight = 0\n") == 0);
    free(f);
    reboot();
    CHECK(settings_get_int(SET_SCREEN_BACKLIGHT, -1) == 96);
    CHECK(settings_get_int(SET_KEYS_BACKLIGHT, -1) == 0);
    CHECK(!settings_dirty());

    // Edited on a PC: comments, odd spacing, CRLF, junk and an unknown key,
    // no newline at the end.
    write_file(SETTINGS_PATH, "# mine\r\n\r\n  screen.backlight=  160 \r\n"
                              "garbage line\r\nfuture.thing = a b c\r\n"
                              "keys.backlight = 64");
    reboot();
    CHECK(settings_get_int(SET_SCREEN_BACKLIGHT, -1) == 160);
    CHECK(settings_get_int(SET_KEYS_BACKLIGHT, -1) == 64);
    CHECK(strcmp(settings_get("future.thing"), "a b c") == 0);
    // Unknown keys survive a rewrite.
    settings_set_int(SET_KEYS_BACKLIGHT, 32);
    CHECK(settings_save() == FR_OK);
    reboot();
    CHECK(strcmp(settings_get("future.thing"), "a b c") == 0);
    CHECK(settings_get_int(SET_KEYS_BACKLIGHT, -1) == 32);

    // A change made before the card's file is read wins over the file.
    settings_clear();
    settings_set_int(SET_KEYS_BACKLIGHT, 224);
    CHECK(settings_load() == FR_OK);
    CHECK(settings_get_int(SET_KEYS_BACKLIGHT, -1) == 224);
    CHECK(settings_get_int(SET_SCREEN_BACKLIGHT, -1) == 160);
    CHECK(settings_dirty());
    CHECK(settings_save() == FR_OK);
    reboot();
    CHECK(settings_get_int(SET_KEYS_BACKLIGHT, -1) == 224);

    // Power lost between removing the old file and renaming the new one.
    write_file(SETTINGS_TMP, "screen.backlight = 48\n");
    CHECK(f_unlink(SETTINGS_PATH) == FR_OK);
    reboot();
    CHECK(settings_get_int(SET_SCREEN_BACKLIGHT, -1) == 48);
    FILINFO fi;
    CHECK(f_stat(SETTINGS_PATH, &fi) == FR_OK && f_stat(SETTINGS_TMP, &fi) == FR_NO_FILE);
    // Lost mid-write of the new one: the old file stands.
    write_file(SETTINGS_TMP, "screen.backlight = 2");
    reboot();
    CHECK(settings_get_int(SET_SCREEN_BACKLIGHT, -1) == 48);
    settings_set_int(SET_SCREEN_BACKLIGHT, 64);
    CHECK(settings_save() == FR_OK); // and it doesn't get in the way
    reboot();
    CHECK(settings_get_int(SET_SCREEN_BACKLIGHT, -1) == 64);

    // A card error leaves it dirty, to try again.
    settings_set_int(SET_SCREEN_BACKLIGHT, 80);
    ramdisk_fail(0);
    CHECK(settings_save() != FR_OK && settings_dirty());
    ramdisk_fail(-1);
    CHECK(f_mount(NULL, "", 0) == FR_OK && f_mount(&fs, "", 1) == FR_OK);
    CHECK(settings_save() == FR_OK && !settings_dirty());
    reboot();
    CHECK(settings_get_int(SET_SCREEN_BACKLIGHT, -1) == 80);
}

int main(void) {
    settings_set_clock(fake_clock);
    test_get_set();
    test_card();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("settings: all tests passed\n");
    return 0;
}
