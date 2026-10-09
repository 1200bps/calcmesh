#include "meshlink.h"

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/uart.h"
#include "pico/rand.h"
#include "pico/stdlib.h"

#include "mesh/link.h"

// The want_config dump arrives back to back at ~11.5 kB/s and the 32-byte
// hardware FIFO lasts under 3 ms, less than one USB printf can take. So the
// RX interrupt moves bytes into this ring and the main loop drains it.
#define RX_RING_SIZE 4096 // power of two

static uint8_t rx_ring[RX_RING_SIZE];
static volatile uint32_t rx_head, rx_tail; // head: IRQ writes, tail: main loop
static volatile uint32_t rx_overruns;

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

void meshlink_task(void) {
    // Hand over contiguous runs of the ring.
    uint32_t head = rx_head;
    while (rx_tail != head) {
        uint32_t tail = rx_tail;
        uint32_t end = head > tail ? head : RX_RING_SIZE;
        mesh_link_rx(&rx_ring[tail], end - tail);
        rx_tail = end & (RX_RING_SIZE - 1);
    }
    mesh_link_poll();
}

uint32_t meshlink_rx_overruns(void) { return rx_overruns; }
