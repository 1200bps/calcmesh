#include "lcd/lcd.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/spi.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#include "ui/fb.h"

// Pins and bus per the ClockworkPi mainboard v2.0 schematic (GP10 SPI1_SCK,
// GP11 SPI1_TX, GP12 SPI1_RX, GP13 SPI1_CS, GP14 LCD_DC, GP15 LCD_RST) and
// their reference firmware (Code/*/lcdspi/lcdspi.h). CS is driven as a
// GPIO so it can stay low across a command and its data.
#ifndef LCD_SPI
#define LCD_SPI spi1
#endif
#define LCD_PIN_SCK 10
#define LCD_PIN_MOSI 11
#define LCD_PIN_MISO 12
#define LCD_PIN_CS 13
#define LCD_PIN_DC 14
#define LCD_PIN_RST 15

// What the reference firmware asks for; the SPI block rounds it down to
// clk_peri / an even divider, so 20.8 MHz on a 125 MHz RP2040 and 25 MHz on
// a 150 MHz RP2350. The ILI9488 datasheet's write cycle is 50 ns, which
// panels routinely beat; raise this once there is glass to try it on.
#ifndef LCD_SPI_HZ
#define LCD_SPI_HZ 25000000
#endif

#define LCD_DMA_IRQ DMA_IRQ_1

enum {
    CMD_SLPOUT = 0x11,
    CMD_INVON = 0x21,
    CMD_DISPON = 0x29,
    CMD_CASET = 0x2A,
    CMD_PASET = 0x2B,
    CMD_RAMWR = 0x2C,
    CMD_MADCTL = 0x36,
    CMD_COLMOD = 0x3A,
};

#define ROW_BYTES (FB_W * 3)

static int dma_ch = -1;
static uint8_t rowbuf[2][ROW_BYTES];

// Queue of row runs waiting their turn. One fb_flush() makes at most
// FB_H / 2 + 1 runs (every other row changed).
#define QLEN (FB_H / 2 + 2)
static struct {
    int16_t y0, y1;
} queue[QLEN];
static volatile uint16_t q_head, q_tail; // head: main loop, tail: DMA IRQ

static volatile bool busy;
static int cur_y, cur_end; // row on the wire, last row of its run
static int cur_buf;        // rowbuf holding cur_y

static inline void cs(bool level) { gpio_put(LCD_PIN_CS, level); }

// Let the FIFO drain before DC changes under it.
static inline void spi_drain(void) {
    while (spi_is_busy(LCD_SPI))
        tight_loop_contents();
}

// A command and its parameters, inside an already-selected transaction.
static void cmd(uint8_t c, const uint8_t *data, size_t len) {
    spi_drain();
    gpio_put(LCD_PIN_DC, 0);
    spi_write_blocking(LCD_SPI, &c, 1);
    gpio_put(LCD_PIN_DC, 1);
    if (len)
        spi_write_blocking(LCD_SPI, data, len);
}

// The same, as a transaction of its own.
static void cmd1(uint8_t c, const uint8_t *data, size_t len) {
    cs(0);
    cmd(c, data, len);
    cs(1);
}

#define CMD(c, ...)                                                            \
    cmd1(c, (const uint8_t[]){__VA_ARGS__}, sizeof((uint8_t[]){__VA_ARGS__}))

// Select, set the window to full-width rows y0..y1, and leave the panel
// expecting pixels with DC high.
static void window(int y0, int y1) {
    const uint8_t cols[] = {0, 0, (FB_W - 1) >> 8, (FB_W - 1) & 0xFF};
    const uint8_t rows[] = {y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF};
    cs(0);
    cmd(CMD_CASET, cols, sizeof cols);
    cmd(CMD_PASET, rows, sizeof rows);
    cmd(CMD_RAMWR, NULL, 0);
}

static void send_row(int b) {
    dma_channel_transfer_from_buffer_now(dma_ch, rowbuf[b], ROW_BYTES);
}

// Both buffers are filled before the first byte goes, so the IRQ never
// finds the next row half-converted.
static void start_run(int y0, int y1) {
    window(y0, y1);
    cur_y = y0;
    cur_end = y1;
    cur_buf = 0;
    fb_row_rgb666(y0, rowbuf[0]);
    if (y0 < y1)
        fb_row_rgb666(y0 + 1, rowbuf[1]);
    send_row(0);
}

static void dma_done(void) {
    if (!dma_channel_get_irq1_status(dma_ch))
        return;
    dma_channel_acknowledge_irq1(dma_ch);

    if (cur_y < cur_end) {
        // Next row is ready in the other buffer; send it, then refill the
        // one that just went out with the row after.
        cur_y++;
        cur_buf ^= 1;
        send_row(cur_buf);
        if (cur_y < cur_end)
            fb_row_rgb666(cur_y + 1, rowbuf[cur_buf ^ 1]);
        return;
    }

    // Run finished: let the last bytes out before deselecting.
    spi_drain();
    cs(1);
    if (q_tail != q_head) {
        int y0 = queue[q_tail].y0, y1 = queue[q_tail].y1;
        q_tail = (uint16_t)((q_tail + 1) % QLEN);
        start_run(y0, y1);
    } else {
        busy = false;
    }
}

void lcd_rows(int y0, int y1, void *ctx) {
    (void)ctx;
    if (y0 < 0)
        y0 = 0;
    if (y1 >= FB_H)
        y1 = FB_H - 1;
    if (y0 > y1)
        return;

    for (;;) {
        uint32_t irq = save_and_disable_interrupts();
        if (!busy) {
            busy = true;
            restore_interrupts(irq);
            start_run(y0, y1);
            return;
        }
        uint16_t next = (uint16_t)((q_head + 1) % QLEN);
        if (next != q_tail) {
            queue[q_head].y0 = (int16_t)y0;
            queue[q_head].y1 = (int16_t)y1;
            q_head = next;
            restore_interrupts(irq);
            return;
        }
        restore_interrupts(irq);
        tight_loop_contents(); // full: let the IRQ make room
    }
}

bool lcd_busy(void) { return busy; }

void lcd_wait(void) {
    while (busy)
        tight_loop_contents();
}

static void reset_panel(void) {
    gpio_put(LCD_PIN_RST, 1);
    sleep_ms(10);
    gpio_put(LCD_PIN_RST, 0);
    sleep_ms(10);
    gpio_put(LCD_PIN_RST, 1);
    sleep_ms(200);
}

// Panel settings from ClockworkPi's pico_lcd_init() (picocalc_helloworld and
// picocalc_lvgl_graphics_demo, lcdspi/lcdspi.c), which is the ILI9488 vendor
// sequence. Gamma, power and VCOM are tuned to the PicoCalc's glass; don't
// change them without the panel in hand.
static void configure(void) {
    CMD(0xE0, 0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78, 0x4C, 0x09,
        0x0A, 0x08, 0x16, 0x1A, 0x0F);                  // positive gamma
    CMD(0xE1, 0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45, 0x46, 0x04,
        0x0E, 0x0D, 0x35, 0x37, 0x0F);                  // negative gamma
    CMD(0xC0, 0x17, 0x15);                              // power control 1
    CMD(0xC1, 0x41);                                    // power control 2
    CMD(0xC5, 0x00, 0x12, 0x80);                        // VCOM
    // MX | BGR: the orientation and colour order the reference uses, with
    // (0, 0) at the top left. The UI is grey, so a wrong BGR bit wouldn't
    // show; a wrong MX would mirror it.
    CMD(CMD_MADCTL, 0x48);
    // 18 bits per pixel: over 4-wire SPI the ILI9488 takes nothing between
    // 3-bit and 18-bit colour. (An ST7365P would also take 0x55, RGB565.)
    CMD(CMD_COLMOD, 0x66);
    CMD(0xB0, 0x00);                                    // interface mode
    CMD(0xB1, 0xA0);                                    // frame rate
    cmd1(CMD_INVON, NULL, 0);                           // IPS: inverted
    CMD(0xB4, 0x02);                                    // 2-dot inversion
    CMD(0xB6, 0x02, 0x02, 0x3B);                        // display function
    CMD(0xB7, 0xC6);                                    // entry mode
    CMD(0xE9, 0x00);                                    // 24-bit bus off
    CMD(0xF7, 0xA9, 0x51, 0x2C, 0x82);                  // adjust control 3
}

// Blocking: only used before the DMA side is running.
static void clear_panel(void) {
    memset(rowbuf[0], 0, ROW_BYTES);
    window(0, FB_H - 1);
    for (int y = 0; y < FB_H; y++)
        spi_write_blocking(LCD_SPI, rowbuf[0], ROW_BYTES);
    spi_drain();
    cs(1);
}

void lcd_init(void) {
    gpio_init(LCD_PIN_CS);
    gpio_set_dir(LCD_PIN_CS, GPIO_OUT);
    gpio_put(LCD_PIN_CS, 1);
    gpio_init(LCD_PIN_DC);
    gpio_set_dir(LCD_PIN_DC, GPIO_OUT);
    gpio_put(LCD_PIN_DC, 1);
    gpio_init(LCD_PIN_RST);
    gpio_set_dir(LCD_PIN_RST, GPIO_OUT);
    gpio_put(LCD_PIN_RST, 1);

    // Mode 0, 8 bits, MSB first (spi_init's defaults). MISO is wired but we
    // never read the panel; muxed anyway, as the reference does.
    unsigned hz = spi_init(LCD_SPI, LCD_SPI_HZ);
    gpio_set_function(LCD_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(LCD_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(LCD_PIN_MISO, GPIO_FUNC_SPI);

    reset_panel();
    configure();
    cmd1(CMD_SLPOUT, NULL, 0);
    sleep_ms(120);
    clear_panel(); // before the panel shows whatever its RAM woke up with
    cmd1(CMD_DISPON, NULL, 0);
    sleep_ms(20);

    if (dma_ch < 0) {
        dma_ch = dma_claim_unused_channel(true);
        dma_channel_config c = dma_channel_get_default_config(dma_ch);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, spi_get_dreq(LCD_SPI, true));
        dma_channel_configure(dma_ch, &c, &spi_get_hw(LCD_SPI)->dr, NULL, 0,
                              false);
        dma_channel_set_irq1_enabled(dma_ch, true);
        irq_add_shared_handler(LCD_DMA_IRQ, dma_done,
                               PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
        irq_set_enabled(LCD_DMA_IRQ, true);
    }

    fb_invalidate();
    printf("[lcd] spi1 at %u Hz\n", hz);
}
