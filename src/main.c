// calcmesh: Meshtastic client for the PicoCalc.
//
// Bring-up firmware for now: runs the want_config handshake with the node
// over uart0, prints the node and channel tables on the USB console, and
// sends and shows text messages. Console commands: `help` lists them.

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "tusb.h"

#include "input/kbd.h"
#include "mesh/cmd.h"
#include "meshlink.h"

static const char *const desktop_names[] = {
    [MESHLINK_DESKTOP_AUTO] = "auto",
    [MESHLINK_DESKTOP_ON] = "on",
    [MESHLINK_DESKTOP_OFF] = "off",
};

static void print_desktop(void) {
    printf("desktop mode %s (%s)\n", desktop_names[meshlink_desktop()],
           meshlink_bridged() ? "bridged" : "not bridged");
}

// Until the UI takes the keyboard, print what it types.
static bool echo_keys = true;

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
    } else if (strcmp(cmd, "help") == 0) {
        mesh_cmd_help();
        printf("desktop [on|off|auto]  bridge the node port to the node\n"
               "                       (auto: while a PC is using it)\n"
               "keys [on|off]          print PicoCalc keypresses here\n");
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

int main(void) {
    // We bring up TinyUSB ourselves for the second CDC port; stdio_usb then
    // uses the first one and leaves tud_task() to us.
    tusb_init();
    stdio_init_all();

    // Time to open the USB console.
    absolute_time_t until = make_timeout_time_ms(2000);
    while (!time_reached(until))
        tud_task();
    printf("calcmesh up\n");

    meshlink_init();
    kbd_init();

    uint32_t overruns = 0;
    for (;;) {
        tud_task();
        meshlink_task();
        console_task();
        kbd_task();
        kbd_event_t ev;
        while (kbd_get(&ev))
            if (echo_keys)
                print_key(&ev);
        if (meshlink_rx_overruns() != overruns) {
            overruns = meshlink_rx_overruns();
            printf("[uart] rx buffer overrun, %lu bytes lost so far\n",
                   (unsigned long)overruns);
        }
    }
}
