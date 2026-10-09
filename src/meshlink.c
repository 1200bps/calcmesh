#include "meshlink.h"

#include <stdio.h>
#include <string.h>

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/uart.h"
#include "pico/rand.h"
#include "pico/stdlib.h"

#include "tusb.h"

#include "mesh/frame.h"
#include "mesh/link.h"
#include "meshtastic/mesh.pb.h"
#include "pb_decode.h"

// The want_config dump arrives back to back at ~11.5 kB/s and the 32-byte
// hardware FIFO lasts under 3 ms, less than one USB printf can take. So the
// RX interrupt moves bytes into this ring and the main loop drains it.
#define RX_RING_SIZE 4096 // power of two

static uint8_t rx_ring[RX_RING_SIZE];
static volatile uint32_t rx_head, rx_tail; // head: IRQ writes, tail: main loop
static volatile uint32_t rx_overruns;

// Auto desktop mode can't rely on DTR alone: the meshtastic Python client
// clears HUPCL so closing the port doesn't reset ESP32 nodes, which leaves
// DTR up after it exits. So a session starts when the PC sends something and
// ends on DTR down, the client's ToRadio.disconnect, or this much silence
// (clients heartbeat every 5 minutes).
#define DESKTOP_IDLE_MS (10 * 60 * 1000)

static meshlink_desktop_t desktop_mode = MESHLINK_DESKTOP_AUTO;
static bool bridged;
static uint32_t pc_last_rx_ms;
static bool pc_disconnected;

// PC -> node frames, decoded only to spot ToRadio.disconnect.
static frame_rx_t pc_rx;
static meshtastic_ToRadio pc_msg;

static void on_uart_rx(void) {
    while (uart_is_readable(MESHLINK_UART)) {
        uint8_t c = (uint8_t)uart_get_hw(MESHLINK_UART)->dr;
        uint32_t next = (rx_head + 1) & (RX_RING_SIZE - 1);
        if (next == rx_tail) {
            rx_overruns++;
            continue;
        }
        rx_ring[rx_head] = c;
        rx_head = next;
    }
}

static void uart_write(const uint8_t *data, size_t len, void *ctx) {
    (void)ctx;
    uart_write_blocking(MESHLINK_UART, data, len);
}

static uint32_t now_ms(void *ctx) {
    (void)ctx;
    return to_ms_since_boot(get_absolute_time());
}

void meshlink_init(void) {
    uart_init(MESHLINK_UART, MESHLINK_BAUD);
    gpio_set_function(MESHLINK_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(MESHLINK_RX_PIN, GPIO_FUNC_UART);
    uart_set_format(MESHLINK_UART, 8, 1, UART_PARITY_NONE);
    uart_set_hw_flow(MESHLINK_UART, false, false);
    uart_set_fifo_enabled(MESHLINK_UART, true);

    int irq = MESHLINK_UART == uart0 ? UART0_IRQ : UART1_IRQ;
    irq_set_exclusive_handler(irq, on_uart_rx);
    irq_set_enabled(irq, true);
    uart_set_irq_enables(MESHLINK_UART, true, false);

    mesh_link_io_t io = {uart_write, now_ms, NULL, get_rand_32()};
    mesh_link_init(&io);
    mesh_link_request_config();
}

static void set_bridged(bool on) {
    if (on == bridged)
        return;
    bridged = on;
    printf("[desktop] %s\n", bridged ? "on: node port bridged to the node, link passive"
                                     : "off: link resuming");
    if (bridged) {
        frame_rx_init(&pc_rx);
        pc_last_rx_ms = to_ms_since_boot(get_absolute_time());
        pc_disconnected = false;
    }
    mesh_link_set_passive(bridged);
}

static void update_bridge(void) {
    bool pc = tud_cdc_n_connected(MESHLINK_USB_CDC);
    switch (desktop_mode) {
    case MESHLINK_DESKTOP_ON:
        set_bridged(true);
        break;
    case MESHLINK_DESKTOP_OFF:
        set_bridged(false);
        break;
    case MESHLINK_DESKTOP_AUTO:
        if (!bridged) {
            set_bridged(pc && tud_cdc_n_available(MESHLINK_USB_CDC));
        } else {
            uint32_t idle = to_ms_since_boot(get_absolute_time()) - pc_last_rx_ms;
            if (!pc || pc_disconnected || idle >= DESKTOP_IDLE_MS)
                set_bridged(false);
        }
        break;
    }
}

static void watch_pc(const uint8_t *buf, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (frame_rx_push(&pc_rx, buf[i]) != FRAME_RX_READY)
            continue;
        pb_istream_t is = pb_istream_from_buffer(pc_rx.buf, pc_rx.len);
        memset(&pc_msg, 0, sizeof pc_msg);
        if (pb_decode(&is, meshtastic_ToRadio_fields, &pc_msg) &&
            pc_msg.which_payload_variant == meshtastic_ToRadio_disconnect_tag)
            pc_disconnected = true;
    }
}

// How much of the next n received bytes can go on now. While bridged with a
// PC listening, node bytes are held in the ring rather than dropped when the
// USB side is full, so the PC sees an intact stream.
static uint32_t usb_room(uint32_t n) {
    if (!bridged || !tud_cdc_n_connected(MESHLINK_USB_CDC))
        return n;
    uint32_t room = tud_cdc_n_write_available(MESHLINK_USB_CDC);
    return n < room ? n : room;
}

void meshlink_task(void) {
    update_bridge();

    // PC -> node. Outside desktop mode, whatever the PC sends is discarded.
    if (bridged) {
        uint8_t buf[64];
        uint32_t n;
        while ((n = tud_cdc_n_read(MESHLINK_USB_CDC, buf, sizeof buf)) > 0) {
            uart_write_blocking(MESHLINK_UART, buf, n);
            pc_last_rx_ms = to_ms_since_boot(get_absolute_time());
            watch_pc(buf, n);
        }
    } else {
        tud_cdc_n_read_flush(MESHLINK_USB_CDC);
    }

    // Node -> link (and the PC). Hand over contiguous runs of the ring.
    uint32_t head = rx_head;
    while (rx_tail != head) {
        uint32_t tail = rx_tail;
        uint32_t end = head > tail ? head : RX_RING_SIZE;
        uint32_t n = usb_room(end - tail);
        if (!n)
            break;
        if (bridged && tud_cdc_n_connected(MESHLINK_USB_CDC))
            tud_cdc_n_write(MESHLINK_USB_CDC, &rx_ring[tail], n);
        mesh_link_rx(&rx_ring[tail], n);
        rx_tail = (tail + n) & (RX_RING_SIZE - 1);
    }
    if (bridged)
        tud_cdc_n_write_flush(MESHLINK_USB_CDC);

    mesh_link_poll();
}

void meshlink_set_desktop(meshlink_desktop_t mode) {
    desktop_mode = mode;
    update_bridge();
}

meshlink_desktop_t meshlink_desktop(void) { return desktop_mode; }
bool meshlink_bridged(void) { return bridged; }

uint32_t meshlink_rx_overruns(void) { return rx_overruns; }
