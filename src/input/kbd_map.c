#include "input/kbd_map.h"

#include <stddef.h>
#include <stdio.h>

// Raw codes from the firmware's keyboard.h.
enum {
    RAW_F1 = 0x81, // ..0x89 = F9
    RAW_F10 = 0x90,
    RAW_MOD_ALT = 0xA1,
    RAW_MOD_SHL = 0xA2,
    RAW_MOD_SHR = 0xA3,
    RAW_MOD_SYM = 0xA4,
    RAW_MOD_CTRL = 0xA5,
    RAW_ESC = 0xB1,
    RAW_LEFT = 0xB4,
    RAW_UP = 0xB5,
    RAW_DOWN = 0xB6,
    RAW_RIGHT = 0xB7,
    RAW_CAPS_LOCK = 0xC1,
    RAW_BREAK = 0xD0,
    RAW_INSERT = 0xD1,
    RAW_HOME = 0xD2,
    RAW_DEL = 0xD4,
    RAW_END = 0xD5,
    RAW_PGUP = 0xD6,
    RAW_PGDN = 0xD7,
};

// The two Shift keys are tracked apart so letting go of one doesn't drop the
// other; folded into KBD_MOD_SHIFT on the way out.
#define MOD_SHL (1u << 4)
#define MOD_SHR (1u << 5)

static uint8_t mod_bit(uint8_t code) {
    switch (code) {
    case RAW_MOD_ALT: return KBD_MOD_ALT;
    case RAW_MOD_CTRL: return KBD_MOD_CTRL;
    case RAW_MOD_SHL: return MOD_SHL;
    case RAW_MOD_SHR: return MOD_SHR;
    default: return 0;
    }
}

static uint16_t translate(uint8_t code) {
    if (code >= 0x20 && code <= 0x7E)
        return code;
    if (code >= RAW_F1 && code <= RAW_F1 + 8)
        return (uint16_t)(KBD_F1 + (code - RAW_F1));
    switch (code) {
    case KBD_BACKSPACE:
    case KBD_TAB:
    case KBD_ENTER: return code;
    case RAW_ESC: return KBD_ESC;
    case RAW_UP: return KBD_UP;
    case RAW_DOWN: return KBD_DOWN;
    case RAW_LEFT: return KBD_LEFT;
    case RAW_RIGHT: return KBD_RIGHT;
    case RAW_HOME: return KBD_HOME;
    case RAW_END: return KBD_END;
    case RAW_PGUP: return KBD_PGUP;
    case RAW_PGDN: return KBD_PGDN;
    case RAW_INSERT: return KBD_INSERT;
    case RAW_DEL: return KBD_DEL;
    case RAW_BREAK: return KBD_BREAK;
    case RAW_F10: return KBD_F10;
    default: return 0; // Caps Lock (the keyboard applies it), Power, Sym
    }
}

// Marks code as held; returns whether it already was.
static bool mark_down(kbd_decoder_t *d, uint8_t code) {
    uint8_t *free_slot = NULL;
    for (size_t i = 0; i < sizeof d->down; i++) {
        if (d->down[i] == code)
            return true;
        if (!d->down[i] && !free_slot)
            free_slot = &d->down[i];
    }
    if (free_slot)
        *free_slot = code;
    return false;
}

static void mark_up(kbd_decoder_t *d, uint8_t code) {
    for (size_t i = 0; i < sizeof d->down; i++)
        if (d->down[i] == code)
            d->down[i] = 0;
}

bool kbd_decode(kbd_decoder_t *d, uint8_t state, uint8_t code, kbd_event_t *ev) {
    if (!code)
        return false;

    uint8_t mod = mod_bit(code);
    if (mod) {
        if (state == KBD_STATE_RELEASED)
            d->mods &= (uint8_t)~mod;
        else
            d->mods |= mod;
        return false;
    }

    bool repeat;
    switch (state) {
    case KBD_STATE_PRESSED:
        // Held printable and arrow keys repeat as more PRESSED entries with
        // no release in between.
        repeat = mark_down(d, code);
        break;
    case KBD_STATE_HOLD:
        // Other keys repeat as HOLD.
        mark_down(d, code);
        repeat = true;
        break;
    case KBD_STATE_RELEASED:
        mark_up(d, code);
        return false;
    default:
        return false;
    }

    uint16_t key = translate(code);
    if (!key)
        return false;

    uint8_t mods = d->mods & (KBD_MOD_CTRL | KBD_MOD_ALT);
    if (d->mods & (MOD_SHL | MOD_SHR))
        mods |= KBD_MOD_SHIFT;
    // With Alt held the keyboard passes letters through as capitals; with
    // Ctrl they're already lower case. Make shortcuts look the same either
    // way, with Shift (if any) left in mods.
    if ((mods & (KBD_MOD_CTRL | KBD_MOD_ALT)) && key >= 'A' && key <= 'Z')
        key = (uint16_t)(key - 'A' + 'a');

    ev->key = key;
    ev->mods = mods;
    ev->repeat = repeat;
    return true;
}

static const char *const special_names[] = {
    "Up", "Down", "Left", "Right", "Home", "End", "PgUp", "PgDn", "Insert",
    "Del", "Break",
};

const char *kbd_key_name(uint16_t key) {
    static char one[2];
    switch (key) {
    case KBD_BACKSPACE: return "Backspace";
    case KBD_TAB: return "Tab";
    case KBD_ENTER: return "Enter";
    case KBD_ESC: return "Esc";
    case ' ': return "Space";
    }
    if (key >= 0x21 && key <= 0x7E) {
        one[0] = (char)key;
        return one;
    }
    if (key >= KBD_UP && key <= KBD_BREAK)
        return special_names[key - KBD_UP];
    if (key >= KBD_F1 && key <= KBD_F10) {
        static char fn[4];
        snprintf(fn, sizeof fn, "F%d", key - KBD_F1 + 1);
        return fn;
    }
    return "?";
}
