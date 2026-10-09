#include "meshlink.h"

#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

#include <pb_decode.h>
#include <pb_encode.h>

#define START1 0x94
#define START2 0xC3
#define MAX_PAYLOAD 512

static enum { WAIT_START1, WAIT_START2, LEN_HI, LEN_LO, PAYLOAD } rx_state;
static uint16_t rx_len, rx_pos;
static uint8_t rx_buf[MAX_PAYLOAD];
static meshlink_log_cb log_cb;

void meshlink_init(void) {
    uart_init(MESHLINK_UART, MESHLINK_BAUD);
    gpio_set_function(MESHLINK_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(MESHLINK_RX_PIN, GPIO_FUNC_UART);
    uart_set_format(MESHLINK_UART, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(MESHLINK_UART, false, false);
    uart_set_fifo_enabled(MESHLINK_UART, true);
    rx_state = WAIT_START1;
}

void meshlink_set_log_cb(meshlink_log_cb cb) {
    log_cb = cb;
}

void meshlink_wake(void) {
    // Same trick the Python client uses: a run of START2 bytes wakes the node
    // and can never be mistaken for the start of a frame.
    for (int i = 0; i < 32; i++)
        uart_putc_raw(MESHLINK_UART, START2);
    sleep_ms(100);
}

bool meshlink_send(const meshtastic_ToRadio *msg) {
    // Static: the core 0 stack is only 2 KiB by default.
    static uint8_t buf[4 + meshtastic_ToRadio_size];
    pb_ostream_t os = pb_ostream_from_buffer(buf + 4, sizeof(buf) - 4);
    if (!pb_encode(&os, meshtastic_ToRadio_fields, msg))
        return false;

    buf[0] = START1;
    buf[1] = START2;
    buf[2] = os.bytes_written >> 8;
    buf[3] = os.bytes_written & 0xff;
    uart_write_blocking(MESHLINK_UART, buf, 4 + os.bytes_written);
    return true;
}

bool meshlink_poll(meshtastic_FromRadio *out) {
    while (uart_is_readable(MESHLINK_UART)) {
        uint8_t c = uart_getc(MESHLINK_UART);

        switch (rx_state) {
        case WAIT_START1:
            if (c == START1)
                rx_state = WAIT_START2;
            else if (log_cb)
                log_cb(c);
            break;
        case WAIT_START2:
            if (c == START2) {
                rx_state = LEN_HI;
            } else if (c == START1) {
                if (log_cb)
                    log_cb(START1);
            } else {
                if (log_cb) {
                    log_cb(START1);
                    log_cb(c);
                }
                rx_state = WAIT_START1;
            }
            break;
        case LEN_HI:
            rx_len = c << 8;
            rx_state = LEN_LO;
            break;
        case LEN_LO:
            rx_len |= c;
            rx_pos = 0;
            // Zero or oversized length means we latched onto garbage; resync.
            rx_state = (rx_len == 0 || rx_len > MAX_PAYLOAD) ? WAIT_START1 : PAYLOAD;
            break;
        case PAYLOAD:
            rx_buf[rx_pos++] = c;
            if (rx_pos == rx_len) {
                rx_state = WAIT_START1;
                pb_istream_t is = pb_istream_from_buffer(rx_buf, rx_len);
                *out = (meshtastic_FromRadio)meshtastic_FromRadio_init_zero;
                if (pb_decode(&is, meshtastic_FromRadio_fields, out))
                    return true;
            }
            break;
        }
    }
    return false;
}
