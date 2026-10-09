// Meshtastic serial (StreamAPI) framing.
//
// Each protobuf travels as: 0x94 0xC3 <len hi> <len lo> <len bytes>, with
// len <= 512. Bytes seen outside a frame are the node's plain-text debug
// console (only present before the node switches the port to API mode, or
// when it has debug logging on), so they are handed back to the caller
// rather than dropped.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define FRAME_START1 0x94
#define FRAME_START2 0xC3
#define FRAME_HEADER_LEN 4
#define FRAME_MAX_PAYLOAD 512

typedef enum {
    FRAME_RX_NONE,  // byte consumed, nothing to report yet
    FRAME_RX_READY, // a full payload is in rx->buf[0..rx->len)
    FRAME_RX_STRAY, // byte was not part of a frame (console text)
} frame_rx_result_t;

typedef struct {
    uint8_t state;
    uint16_t len;
    uint16_t pos;
    uint32_t dropped; // frames discarded for an impossible length
    uint8_t buf[FRAME_MAX_PAYLOAD];
} frame_rx_t;

void frame_rx_init(frame_rx_t *rx);

// Feed one byte. On FRAME_RX_READY the payload stays valid until the next call.
frame_rx_result_t frame_rx_push(frame_rx_t *rx, uint8_t b);

// Write the 4-byte header for a payload of len bytes into hdr. Returns 0, or
// -1 if len is too large to frame.
int frame_header(uint8_t hdr[FRAME_HEADER_LEN], size_t len);
