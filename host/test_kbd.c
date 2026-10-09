// Feeds keyboard FIFO entries, as the PicoCalc's STM32 reports them, through
// the decoder.
#include <stdio.h>
#include <string.h>

#include "input/kbd_map.h"

static int failures;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
            failures++;                                                        \
        }                                                                      \
    } while (0)

enum { P = KBD_STATE_PRESSED, H = KBD_STATE_HOLD, R = KBD_STATE_RELEASED };

static kbd_decoder_t dec;
static kbd_event_t ev;

static bool feed(uint8_t state, uint8_t code) {
    memset(&ev, 0, sizeof ev);
    return kbd_decode(&dec, state, code, &ev);
}

static void test_plain_and_repeat(void) {
    memset(&dec, 0, sizeof dec);
    CHECK(feed(P, 'a') && ev.key == 'a' && ev.mods == 0 && !ev.repeat);
    // Held printable keys repeat as further PRESSED entries.
    CHECK(feed(P, 'a') && ev.key == 'a' && ev.repeat);
    CHECK(!feed(R, 'a'));
    CHECK(feed(P, 'a') && !ev.repeat);
    CHECK(!feed(R, 'a'));
    CHECK(!feed(KBD_STATE_IDLE, 0));
}

static void test_specials(void) {
    memset(&dec, 0, sizeof dec);
    CHECK(feed(P, 0xB5) && ev.key == KBD_UP);
    CHECK(feed(P, 0xB1) && ev.key == KBD_ESC && !ev.repeat);
    // Non-printable keys repeat as HOLD.
    CHECK(feed(H, 0xB1) && ev.key == KBD_ESC && ev.repeat);
    CHECK(feed(P, 0x0A) && ev.key == KBD_ENTER);
    CHECK(feed(P, 0x08) && ev.key == KBD_BACKSPACE);
    CHECK(feed(P, 0x81) && ev.key == KBD_F1);
    CHECK(feed(P, 0x89) && ev.key == KBD_F1 + 8);
    CHECK(feed(P, 0x90) && ev.key == KBD_F10);
    CHECK(feed(P, 0xD7) && ev.key == KBD_PGDN);
    CHECK(feed(P, 0xD0) && ev.key == KBD_BREAK);
    CHECK(!feed(P, 0xC1)); // Caps Lock: the keyboard applies it itself
    CHECK(!feed(P, 0x91)); // Power
}

static void test_mods(void) {
    memset(&dec, 0, sizeof dec);
    CHECK(!feed(P, 0xA5)); // Ctrl down
    CHECK(feed(P, 'c') && ev.key == 'c' && ev.mods == KBD_MOD_CTRL);
    CHECK(!feed(H, 0xA5)); // held modifiers report HOLD; still not an event
    CHECK(!feed(R, 'c'));
    CHECK(!feed(R, 0xA5));
    CHECK(feed(P, 'c') && ev.mods == 0);

    // Alt passes letters through as capitals; we fold them.
    CHECK(!feed(P, 0xA1));
    CHECK(feed(P, 'X') && ev.key == 'x' && ev.mods == KBD_MOD_ALT);
    CHECK(!feed(R, 0xA1));

    // Two shifts: letting go of one keeps Shift.
    CHECK(!feed(P, 0xA2));
    CHECK(!feed(P, 0xA3));
    CHECK(!feed(R, 0xA2));
    CHECK(feed(P, 'Q') && ev.key == 'Q' && ev.mods == KBD_MOD_SHIFT);
    CHECK(!feed(R, 0xA3));
    CHECK(feed(P, 'q') && ev.mods == 0);

    // Ctrl+Shift+letter keeps Shift in mods, letter lower case.
    CHECK(!feed(P, 0xA5));
    CHECK(!feed(P, 0xA2));
    CHECK(feed(P, 'Z') && ev.key == 'z' &&
          ev.mods == (KBD_MOD_CTRL | KBD_MOD_SHIFT));
}

static void test_names(void) {
    CHECK(strcmp(kbd_key_name('a'), "a") == 0);
    CHECK(strcmp(kbd_key_name(' '), "Space") == 0);
    CHECK(strcmp(kbd_key_name(KBD_ENTER), "Enter") == 0);
    CHECK(strcmp(kbd_key_name(KBD_BREAK), "Break") == 0);
    CHECK(strcmp(kbd_key_name(KBD_F10), "F10") == 0);
}

int main(void) {
    test_plain_and_repeat();
    test_specials();
    test_mods();
    test_names();
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    return failures ? 1 : 0;
}
