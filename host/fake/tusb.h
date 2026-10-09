// Stand-in for the few TinyUSB CDC calls screenport.c makes (fake_tusb.c).
#pragma once

#include <stdbool.h>
#include <stdint.h>

bool tud_cdc_n_connected(uint8_t itf);
uint32_t tud_cdc_n_write_available(uint8_t itf);
uint32_t tud_cdc_n_write(uint8_t itf, void const *buffer, uint32_t bufsize);
uint32_t tud_cdc_n_write_flush(uint8_t itf);
bool tud_cdc_n_write_clear(uint8_t itf);
uint32_t tud_cdc_n_read_flush(uint8_t itf);
