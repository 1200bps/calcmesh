// The PicoCalc's microSD card: mounting, card detect, and the message
// history on it (msglog.h). Without a card everything carries on in RAM.
//
// main() calls sd_init() once, before the main loop starts handling the
// link (it loads saved messages into the store), then sd_task() every pass.
#pragma once

#include <stdbool.h>

// spi0, per the ClockworkPi mainboard v2.0 schematic.
#define SD_SPI spi0
#define SD_MISO_PIN 16
#define SD_CS_PIN 17
#define SD_SCK_PIN 18
#define SD_MOSI_PIN 19
#define SD_DET_PIN 22 // low while a card is in

typedef enum {
    SD_NO_CARD,
    SD_READY, // mounted, history logging
    SD_ERROR, // a card is in but won't mount or write; retried now and then
} sd_state_t;

// Mount the card if there is one and restore saved messages. Blocks while
// it reads them (up to ~1 s; no card: a few ms).
void sd_init(void);
// Card detect, remounting, and writing queued history, a record at a time.
void sd_task(void);

sd_state_t sd_state(void);
// Card, filesystem and logging state on the console.
void sd_print_status(void);

// ---- sd_card.c (FatFs disk driver) -----------------------------------------
void sd_card_pins_init(void);
void sd_card_eject(void);
const char *sd_card_type(void);
