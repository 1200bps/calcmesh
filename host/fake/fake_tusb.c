#include "fake_tusb.h"

#include <string.h>

bool fake_cdc_open;
uint8_t fake_cdc_buf[FAKE_CDC_BUFSIZE];
uint32_t fake_cdc_len;
uint8_t fake_cdc_rx[FAKE_CDC_BUFSIZE];
uint32_t fake_cdc_rx_len;

bool tud_cdc_n_connected(uint8_t itf) {
    (void)itf;
    return fake_cdc_open;
}

uint32_t tud_cdc_n_write_available(uint8_t itf) {
    (void)itf;
    return FAKE_CDC_BUFSIZE - fake_cdc_len;
}

uint32_t tud_cdc_n_write(uint8_t itf, void const *buffer, uint32_t bufsize) {
    uint32_t n = tud_cdc_n_write_available(itf);
    if (bufsize < n)
        n = bufsize;
    memcpy(fake_cdc_buf + fake_cdc_len, buffer, n);
    fake_cdc_len += n;
    return n;
}

uint32_t tud_cdc_n_write_flush(uint8_t itf) {
    (void)itf;
    return 0;
}

bool tud_cdc_n_write_clear(uint8_t itf) {
    (void)itf;
    fake_cdc_len = 0;
    return true;
}

uint32_t tud_cdc_n_read(uint8_t itf, void *buffer, uint32_t bufsize) {
    (void)itf;
    uint32_t n = fake_cdc_rx_len < bufsize ? fake_cdc_rx_len : bufsize;
    memcpy(buffer, fake_cdc_rx, n);
    memmove(fake_cdc_rx, fake_cdc_rx + n, fake_cdc_rx_len - n);
    fake_cdc_rx_len -= n;
    return n;
}

uint32_t fake_cdc_drain(uint8_t *out, uint32_t max) {
    uint32_t n = fake_cdc_len < max ? fake_cdc_len : max;
    memcpy(out, fake_cdc_buf, n);
    memmove(fake_cdc_buf, fake_cdc_buf + n, fake_cdc_len - n);
    fake_cdc_len -= n;
    return n;
}
