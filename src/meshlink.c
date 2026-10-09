#include "meshlink.h"

#include <stdio.h>

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/uart.h"
#include "pico/rand.h"
#include "pico/stdlib.h"

#include "tusb.h"

#include "mesh/link.h"

// The want_config dump arrives back to back at ~11.5 kB/s and the 32-byte
// hardware FIFO lasts under 3 ms, less than one USB printf can take. So the
// RX interrupt moves bytes into this ring and the main loop drains it.
#define RX_RING_SIZE 4096 // power of two

static uint8_t rx_ring[RX_RING_SIZE];
static volatile uint32_t rx_head, rx_tail; // head: IRQ writes, tail: main loop
static volatile uint32_t rx_overruns;

static meshlink_desktop_t desktop_mode = MESHLINK_DESKTOP_AUTO;
static bool bridged;

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

static void update_bridge(void) {
    bool want = desktop_mode == MESHLINK_DESKTOP_ON ||
                (desktop_mode == MESHLINK_DESKTOP_AUTO && tud_cdc_n_connected(MESHLINK_USB_CDC));
    if (want == bridged)
        return;
    bridged = want;
    printf("[desktop] %s\n", bridged ? "on: node port bridged to the node, link passive"
                                     : "off: link resuming");
    mesh_link_set_passive(bridged);
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
        while ((n = tud_cdc_n_read(MESHLINK_USB_CDC, buf, sizeof buf)) > 0)
            uart_write_blocking(MESHLINK_UART, buf, n);
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
