#include "frame.h"

enum { ST_START1, ST_START2, ST_LEN_HI, ST_LEN_LO, ST_PAYLOAD };

void frame_rx_init(frame_rx_t *rx) {
    rx->state = ST_START1;
    rx->len = 0;
    rx->pos = 0;
    rx->dropped = 0;
}

frame_rx_result_t frame_rx_push(frame_rx_t *rx, uint8_t b) {
    switch (rx->state) {
    case ST_START1:
        if (b == FRAME_START1) {
            rx->state = ST_START2;
            return FRAME_RX_NONE;
        }
        return FRAME_RX_STRAY;

    case ST_START2:
        if (b == FRAME_START2) {
            rx->state = ST_LEN_HI;
            return FRAME_RX_NONE;
        }
        // False start. The 0x94 is lost as console text, which is harmless;
        // this byte may itself begin a frame.
        rx->state = (b == FRAME_START1) ? ST_START2 : ST_START1;
        return (b == FRAME_START1) ? FRAME_RX_NONE : FRAME_RX_STRAY;

    case ST_LEN_HI:
        rx->len = (uint16_t)b << 8;
        rx->state = ST_LEN_LO;
        return FRAME_RX_NONE;

    case ST_LEN_LO:
        rx->len |= b;
        rx->pos = 0;
        if (rx->len > FRAME_MAX_PAYLOAD) {
            // Corrupt header; resync on the next start byte.
            rx->dropped++;
            rx->state = ST_START1;
            return FRAME_RX_NONE;
        }
        if (rx->len == 0) {
            // An empty protobuf is a valid (all-defaults) message.
            rx->state = ST_START1;
            return FRAME_RX_READY;
        }
        rx->state = ST_PAYLOAD;
        return FRAME_RX_NONE;

    case ST_PAYLOAD:
    default:
        rx->buf[rx->pos++] = b;
        if (rx->pos >= rx->len) {
            rx->state = ST_START1;
            return FRAME_RX_READY;
        }
        return FRAME_RX_NONE;
    }
}

int frame_header(uint8_t hdr[FRAME_HEADER_LEN], size_t len) {
    if (len > FRAME_MAX_PAYLOAD)
        return -1;
    hdr[0] = FRAME_START1;
    hdr[1] = FRAME_START2;
    hdr[2] = (uint8_t)(len >> 8);
    hdr[3] = (uint8_t)len;
    return 0;
}
