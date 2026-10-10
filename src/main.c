// calcmesh: Meshtastic client for the PicoCalc.
//
// Runs the want_config handshake with the node over uart0 and drives the UI
// (src/ui) from the PicoCalc keyboard. The screen goes to the LCD
// (lcd/lcd.h) and is mirrored over USB (screenport.h; tools/screen.py shows
// it). The USB console logs the node and channel tables and messages, and
// takes commands: `help` lists them. Messages are saved to the SD card
// (sd/sd.h) when there is one.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "tusb.h"

#include "audio/chime.h"
#include "input/backlight.h"
#include "input/kbd.h"
#include "lcd/lcd.h"
#include "mesh/cmd.h"
#include "mesh/metrics.h"
#include "mesh/nodeedit.h"
#include "meshlink.h"
#include "screenport.h"
#include "sd/sd.h"
#include "sd/settings.h"
#include "ui/fb.h"
#include "ui/ui.h"

static const char *const desktop_names[] = {
    [MESHLINK_DESKTOP_AUTO] = "auto",
    [MESHLINK_DESKTOP_ON] = "on",
    [MESHLINK_DESKTOP_OFF] = "off",
};

#define IDLE_DEFAULT 10
#define IDLE_MIN 6 // clients heartbeat every 5 minutes
#define IDLE_MAX 120

static void print_desktop(void) {
    printf("desktop mode %s (%s)", desktop_names[meshlink_desktop()],
           meshlink_bridged() ? "bridged" : "not bridged");
    if (meshlink_desktop() == MESHLINK_DESKTOP_AUTO)
        printf(", ends after %ld min idle",
               (long)settings_get_int(SET_DESKTOP_IDLE, IDLE_DEFAULT));
    printf("\n");
}

// Settings (sd/settings.h) that main.c puts into effect; backlight.c does
// the backlights. Also run when the table is loaded from a card.
static bool echo_keys; // also print keypresses on the console

static void apply_settings(void) {
    static uint32_t seen = (uint32_t)-1;
    if (settings_version() == seen)
        return;
    seen = settings_version();
    const char *m = settings_get(SET_DESKTOP_MODE);
    meshlink_desktop_t mode = MESHLINK_DESKTOP_AUTO;
    for (size_t i = 0; m && i < count_of(desktop_names); i++)
        if (strcmp(m, desktop_names[i]) == 0)
            mode = (meshlink_desktop_t)i;
    if (mode != meshlink_desktop())
        meshlink_set_desktop(mode);
    int32_t idle = settings_get_int(SET_DESKTOP_IDLE, IDLE_DEFAULT);
    meshlink_set_desktop_idle_min(idle < IDLE_MIN ? IDLE_MIN : idle > IDLE_MAX ? IDLE_MAX : (unsigned)idle);
    echo_keys = settings_get_bool(SET_KEY_ECHO, false);
}

static void print_key(const kbd_event_t *ev) {
    printf("[kbd] %s%s%s%s%s\n", ev->mods & KBD_MOD_CTRL ? "Ctrl+" : "",
           ev->mods & KBD_MOD_ALT ? "Alt+" : "",
           ev->mods & KBD_MOD_SHIFT ? "Shift+" : "", kbd_key_name(ev->key),
           ev->repeat ? " (repeat)" : "");
}

static void run_command(char *line) {
    if (mesh_cmd_run(line))
        return;
    char *cmd = strtok(line, " \t");
    char *arg = strtok(NULL, " \t");
    if (!cmd)
        return;

    if (strcmp(cmd, "desktop") == 0) {
        char *end;
        long min = arg ? strtol(arg, &end, 10) : 0;
        if (arg && end != arg && !*end) {
            if (min < IDLE_MIN || min > IDLE_MAX) {
                printf("idle timeout is %d-%d min\n", IDLE_MIN, IDLE_MAX);
                return;
            }
            settings_set_int(SET_DESKTOP_IDLE, (int32_t)min); // saved, too
            apply_settings();
        } else if (arg) {
            size_t i;
            for (i = 0; i < count_of(desktop_names); i++)
                if (strcmp(arg, desktop_names[i]) == 0)
                    break;
            if (i == count_of(desktop_names)) {
                printf("usage: desktop [on|off|auto|MINUTES]\n");
                return;
            }
            settings_set(SET_DESKTOP_MODE, desktop_names[i]); // saved, too
            apply_settings();
        }
        print_desktop();
    } else if (strcmp(cmd, "keys") == 0) {
        if (arg && (strcmp(arg, "on") == 0 || strcmp(arg, "off") == 0))
            settings_set_bool(SET_KEY_ECHO, arg[1] == 'n'), apply_settings();
        else if (arg) {
            printf("usage: keys [on|off]\n");
            return;
        }
        printf("key echo %s, keyboard %s\n", echo_keys ? "on" : "off",
               kbd_online() ? "online" : "not answering");
    } else if (strcmp(cmd, "chime") == 0) {
        static const char *const kinds[CHIME_KINDS] = {"dm", "ch"};
        static const char *const keys[CHIME_KINDS] = {SET_CHIME_DM, SET_CHIME_CHANNEL};
        char *onoff = strtok(NULL, " \t");
        int k = arg && strcmp(arg, "dm") == 0 ? CHIME_DM : arg && strcmp(arg, "ch") == 0 ? CHIME_CHANNEL : -1;
        if (arg && (k < 0 || (onoff && strcmp(onoff, "on") && strcmp(onoff, "off")))) {
            printf("usage: chime [dm|ch] [on|off]\n");
            return;
        }
        if (k >= 0 && onoff)
            settings_set_bool(keys[k], onoff[1] == 'n');
        else if (k >= 0)
            chime_play((chime_kind_t)k);
        chime_task(); // takes up the change
        for (int i = 0; i < CHIME_KINDS; i++)
            printf("%s chime %s%s", kinds[i], chime_enabled((chime_kind_t)i) ? "on" : "off",
                   i + 1 < CHIME_KINDS ? ", " : "\n");
    } else if (strcmp(cmd, "sd") == 0) {
        sd_print_status();
    } else if (strcmp(cmd, "bl") == 0 || strcmp(cmd, "kbl") == 0) {
        bool keys = cmd[0] == 'k';
        if (arg) {
            char *end;
            long v = strtol(arg, &end, 0);
            if (*end || v < 0 || v > 255) {
                printf("usage: %s [0-255]\n", cmd);
                return;
            }
            if (keys)
                kbd_set_key_backlight((uint8_t)v);
            else
                kbd_set_lcd_backlight((uint8_t)v);
            printf("%s backlight set to %ld; the keyboard rounds it, and it's saved\n",
                   keys ? "keyboard" : "screen", v);
        } else {
            printf("%s backlight %d\n", keys ? "keyboard" : "screen",
                   keys ? kbd_key_backlight() : kbd_lcd_backlight());
        }
    } else if (strcmp(cmd, "bat") == 0) {
        bool chg;
        int pct = kbd_battery(&chg);
        if (pct < 0)
            printf("battery not read yet\n");
        else
            printf("battery %d%%%s\n", pct, chg ? ", charging" : "");
    } else if (strcmp(cmd, "help") == 0) {
        mesh_cmd_help();
        printf("desktop [on|off|auto]  bridge the node port to the node\n"
               "                       (auto: while a PC is using it)\n"
               "desktop MINUTES        auto mode's idle timeout (6-120)\n"
               "keys [on|off]          also print PicoCalc keypresses here\n"
               "chime [dm|ch]          play the DM (RR) or channel (C) chime\n"
               "chime dm|ch on|off     switch one on or off (saved)\n"
               "sd                     SD card and message history state\n"
               "bl [0-255]             screen backlight (steps of 16, 16-240)\n"
               "kbl [0-255]            keyboard backlight (steps of 32, 0 = off)\n"
               "                       (desktop, keys, bl and kbl are saved, as\n"
               "                       on the Settings tab)\n"
               "bat                    PicoCalc battery\n");
    } else {
        printf("unknown command '%s', try help\n", cmd);
    }
}

// Line editing for the USB console: echo, backspace, run on Enter. Takes
// UTF-8; backspace removes a whole character.
static void console_task(void) {
    static char line[256];
    static size_t len;

    int c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (c == '\r' || c == '\n') {
            if (!len)
                continue;
            putchar('\n');
            line[len] = '\0';
            len = 0;
            run_command(line);
        } else if ((c == '\b' || c == 0x7f) && len) {
            while (len && ((uint8_t)line[--len] & 0xC0) == 0x80)
                ;
            printf("\b \b");
        } else if (c >= 0x20 && c != 0x7f && len < sizeof line - 1) {
            line[len++] = (char)c;
            putchar(c);
        }
    }
}

// The displays. fb_flush() reports each changed row once, so every display
// takes it from show_rows(). The LCD reads the framebuffer out by DMA after
// lcd_rows() returns, so nothing redraws until it is done.
static void show_rows(int y0, int y1, void *ctx) {
    lcd_rows(y0, y1, ctx);
    screenport_rows(y0, y1, ctx);
}

static bool displays_idle(void) { return !lcd_busy(); }

// Redraw right after a keypress, otherwise this often, for things that change
// on their own (incoming messages, link state, "heard 5m ago").
#define UI_REFRESH_MS 100

static void ui_task(void) {
    static absolute_time_t next;
    static bool due = true;
    kbd_event_t ev;
    while (kbd_get(&ev)) {
        if (echo_keys)
            print_key(&ev);
        ui_key(&ev);
        due = true;
    }
    if (time_reached(next))
        due = true;
    // Not while a display is still reading the framebuffer out
    if (!due || !displays_idle())
        return;
    due = false;
    next = make_timeout_time_ms(UI_REFRESH_MS);
    // The clock for "heard 5m ago" is the node's (mesh_link_epoch()), which
    // the UI reads by itself; ui_set_now() is for a better source, if one
    // turns up.
    bool chg;
    int pct = kbd_battery(&chg);
    ui_set_battery(pct, chg);
    ui_render();
    fb_flush(show_rows, NULL);
}

int main(void) {
    // We bring up TinyUSB ourselves for the extra CDC ports; stdio_usb then
    // uses the first one and leaves tud_task() to us.
    tusb_init();
    stdio_init_all();

    // Time to open the USB console.
    absolute_time_t until = make_timeout_time_ms(2000);
    while (!time_reached(until))
        tud_task();
    printf("calcmesh up\n");

    sd_init(); // before the link: loads settings and saved messages
    meshlink_init();
    kbd_init();
    backlight_init(); // saved levels, from the settings
    lcd_init();
    ui_init();
    chime_init();

    uint32_t overruns = 0;
    for (;;) {
        tud_task();
        meshlink_task();
        console_task();
        kbd_task();
        backlight_task();
        sd_task();
        apply_settings();
        chime_task();
        mesh_edit_poll();
        mesh_metrics_poll();
        ui_task();
        screenport_task();
        if (meshlink_rx_overruns() != overruns) {
            overruns = meshlink_rx_overruns();
            printf("[uart] rx buffer overrun, %lu bytes lost so far\n",
                   (unsigned long)overruns);
        }
    }
}
