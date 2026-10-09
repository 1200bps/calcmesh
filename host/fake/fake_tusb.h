// The stand-in USB port's transmit buffer, same size as the firmware's.
#pragma once

#include "tusb.h"

#define FAKE_CDC_BUFSIZE 1024

extern bool fake_cdc_open; // whether "the PC" has the port open
extern uint8_t fake_cdc_buf[FAKE_CDC_BUFSIZE];
extern uint32_t fake_cdc_len;

// Take up to max bytes out of the transmit buffer, as the PC reading would.
uint32_t fake_cdc_drain(uint8_t *out, uint32_t max);
