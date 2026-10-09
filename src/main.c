// calcmesh: Meshtastic client for the PicoCalc.
//
// Bring-up firmware for now: runs the want_config handshake with the node
// over uart0 and prints the node and channel tables on the USB console.

#include <stdio.h>

#include "pico/stdlib.h"

#include "meshlink.h"

int main(void) {
    stdio_init_all();

    sleep_ms(2000); // time to open the USB console
    printf("calcmesh up\n");

    meshlink_init();

    uint32_t overruns = 0;
    for (;;) {
        meshlink_task();
        if (meshlink_rx_overruns() != overruns) {
            overruns = meshlink_rx_overruns();
            printf("[uart] rx buffer overrun, %lu bytes lost so far\n",
                   (unsigned long)overruns);
        }
        tight_loop_contents();
    }
}
