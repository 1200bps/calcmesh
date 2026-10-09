// calcmesh: Meshtastic client for the PicoCalc.
//
// Runs the want_config handshake with the node over uart0 and drives the UI
// (src/ui) from the PicoCalc keyboard. The screen goes to the LCD
// (lcd/lcd.h) and is mirrored over USB (screenport.h; tools/screen.py shows
// it). The USB console logs the node and channel tables and messages, and
// takes commands: `help` lists them. Messages are saved to the SD card
// (sd/sd.h) when there is one.

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "tusb.h"

#include "input/kbd.h"
#include "lcd/lcd.h"
#include "mesh/cmd.h"
#include "meshlink.h"
#include "screenport.h"
#include "sd/sd.h"
#include "ui/fb.h"
#include "ui/ui.h"

static const char *const desktop_names[] = {
    [MESHLINK_DESKTOP_AUTO] = "auto",
    [MESHLINK_DESKTOP_ON] = "on",
    [MESHLINK_DESKTOP_OFF] = "off",
};

static void print_desktop(void) {
    printf("desktop mode %s (%s)\n", desktop_names[meshlink_desktop()],
           meshlink_bridged() ? "bridged" : "not bridged");
}

// Also print keypresses on the console (they go to the UI either way).
static bool echo_keys;

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
        if (arg) {
            size_t i;
            for (i = 0; i < count_of(desktop_names); i++)
                if (strcmp(arg, desktop_names[i]) == 0)
                    break;
            if (i == count_of(desktop_names)) {
                printf("usage: desktop [on|off|auto]\n");
                return;
            }
            meshlink_set_desktop((meshlink_desktop_t)i);
        }
        print_desktop();
    } else if (strcmp(cmd, "keys") == 0) {
        if (arg && strcmp(arg, "on") == 0)
            echo_keys = true;
        else if (arg && strcmp(arg, "off") == 0)
            echo_keys = false;
        else if (arg) {
            printf("usage: keys [on|off]\n");
            return;
        }
        printf("key echo %s, keyboard %s\n", echo_keys ? "on" : "off",
               kbd_online() ? "online" : "not answering");
    } else if (strcmp(cmd, "sd") == 0) {
        sd_print_status();
    } else if (strcmp(cmd, "help") == 0) {
        mesh_cmd_help();
        printf("desktop [on|off|auto]  bridge the node port to the node\n"
               "                       (auto: while a PC is using it)\n"
               "keys [on|off]          also print PicoCalc keypresses here\n"
               "sd                     SD card and message history state\n");
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

    sd_init(); // before the link: loads saved messages into the store
    meshlink_init();
    kbd_init();
    lcd_init();
    ui_init();

    uint32_t overruns = 0;
    for (;;) {
        tud_task();
        meshlink_task();
        console_task();
        kbd_task();
        sd_task();
        ui_task();
        screenport_task();
        if (meshlink_rx_overruns() != overruns) {
            overruns = meshlink_rx_overruns();
            printf("[uart] rx buffer overrun, %lu bytes lost so far\n",
                   (unsigned long)overruns);
        }
    }
}
