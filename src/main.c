// calcmesh: Meshtastic client for the PicoCalc.
//
// Bring-up firmware for now: runs the want_config handshake with the node
// over uart0 and prints the node and channel tables on the USB console.
// Console commands: `desktop [on|off|auto]`, `help`.

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "tusb.h"

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

static void run_command(char *line) {
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
    } else if (strcmp(cmd, "help") == 0) {
        printf("desktop [on|off|auto]  bridge the node port to the node\n"
               "                       (auto: while a PC is using it)\n");
    } else {
        printf("unknown command '%s', try help\n", cmd);
    }
}

// Line editing for the USB console: echo, backspace, run on Enter.
static void console_task(void) {
    static char line[64];
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
            len--;
            printf("\b \b");
        } else if (c >= 0x20 && c < 0x7f && len < sizeof line - 1) {
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

    uint32_t overruns = 0;
    for (;;) {
        tud_task();
        meshlink_task();
        console_task();
        if (meshlink_rx_overruns() != overruns) {
            overruns = meshlink_rx_overruns();
            printf("[uart] rx buffer overrun, %lu bytes lost so far\n",
                   (unsigned long)overruns);
        }
    }
}
