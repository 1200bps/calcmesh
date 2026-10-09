// Keyboard polling. Each poll is two I2C transactions, a register write
// (0x09, the FIFO) and a 2-byte read of [state, key]. At 10 kHz that's ~5 ms
// of bus time, so instead of the SDK's blocking calls we queue the bytes in
// the I2C block's own command FIFO and check back on later kbd_task() calls.

#include "input/kbd.h"

#include <stdio.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/time.h"

#include "input/kbd_map.h"

#define REG_FIFO 0x09

#define POLL_MS 10          // the keyboard scans its matrix every 16 ms
#define GAP_US 1000         // after the register write, before the read
#define XFER_TIMEOUT_MS 50  // a transaction stuck longer than this resets the bus
#define RETRY_MS 500        // after a NACK (keyboard absent or still booting)

typedef enum {
    ST_WAIT,  // until `at`, then write the register
    ST_WRITE, // register write in flight
    ST_GAP,   // until `at`, then start the read
    ST_READ,  // read in flight
} state_t;

static struct {
    state_t state;
    absolute_time_t at;
    absolute_time_t started;
    kbd_decoder_t dec;
    kbd_event_t queue[16];
    uint8_t head, count;
    bool online;
    bool ever_online;
} kbd;

static i2c_hw_t *hw(void) { return i2c_get_hw(KBD_I2C); }

// Frees a bus held by a slave stuck mid-byte (SDA low): clock SCL until it
// lets go, then send a STOP.
static void bus_recover(void) {
    gpio_init(KBD_SDA_PIN);
    gpio_init(KBD_SCL_PIN);
    gpio_pull_up(KBD_SDA_PIN);
    gpio_pull_up(KBD_SCL_PIN);
    gpio_set_dir(KBD_SDA_PIN, GPIO_IN);
    gpio_set_dir(KBD_SCL_PIN, GPIO_IN); // released = high via pull-up
    for (int i = 0; i < 9 && !gpio_get(KBD_SDA_PIN); i++) {
        gpio_put(KBD_SCL_PIN, 0);
        gpio_set_dir(KBD_SCL_PIN, GPIO_OUT);
        busy_wait_us(50);
        gpio_set_dir(KBD_SCL_PIN, GPIO_IN);
        busy_wait_us(50);
    }
    // STOP: SDA low -> high while SCL is high.
    gpio_put(KBD_SDA_PIN, 0);
    gpio_set_dir(KBD_SDA_PIN, GPIO_OUT);
    busy_wait_us(50);
    gpio_set_dir(KBD_SDA_PIN, GPIO_IN);
    busy_wait_us(50);
}

static void bus_init(void) {
    i2c_init(KBD_I2C, KBD_BAUD);
    gpio_set_function(KBD_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(KBD_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(KBD_SDA_PIN);
    gpio_pull_up(KBD_SCL_PIN);

    // One device, so set the target address once (the SDK's transfer calls
    // would do this every time).
    hw()->enable = 0;
    while (hw()->enable_status & I2C_IC_ENABLE_STATUS_IC_EN_BITS)
        tight_loop_contents();
    hw()->tar = KBD_ADDR;
    hw()->enable = 1;
}

static void bus_reset(void) {
    i2c_deinit(KBD_I2C);
    bus_recover();
    bus_init();
}

static void wait_until(state_t next, uint32_t us) {
    kbd.state = next;
    kbd.at = make_timeout_time_us(us);
}

static void start(state_t next) {
    // Clear what the last transaction left behind.
    (void)hw()->clr_stop_det;
    while (hw()->rxflr)
        (void)hw()->data_cmd;

    if (next == ST_WRITE) {
        hw()->data_cmd = REG_FIFO | I2C_IC_DATA_CMD_STOP_BITS;
    } else {
        hw()->data_cmd = I2C_IC_DATA_CMD_CMD_BITS;
        hw()->data_cmd = I2C_IC_DATA_CMD_CMD_BITS | I2C_IC_DATA_CMD_STOP_BITS;
    }
    kbd.state = next;
    kbd.started = get_absolute_time();
}

static void set_online(bool online) {
    if (online == kbd.online)
        return;
    kbd.online = online;
    if (online) {
        printf("[kbd] keyboard %s\n", kbd.ever_online ? "back" : "found");
        kbd.ever_online = true;
    } else {
        printf("[kbd] keyboard not answering\n");
    }
}

static void push(const kbd_event_t *ev) {
    if (kbd.count == count_of(kbd.queue))
        return; // nobody's reading; drop the newest
    kbd.queue[(kbd.head + kbd.count++) % count_of(kbd.queue)] = *ev;
}

// One [state, key] entry arrived. Returns whether the FIFO had something, in
// which case there may be more behind it.
static bool got_entry(uint8_t state, uint8_t code) {
    if (state == KBD_STATE_IDLE && code == 0)
        return false;
    kbd_event_t ev;
    if (kbd_decode(&kbd.dec, state, code, &ev))
        push(&ev);
    return true;
}

void kbd_init(void) {
    bus_init();
    wait_until(ST_WAIT, 0);
}

void kbd_task(void) {
    switch (kbd.state) {
    case ST_WAIT:
        if (time_reached(kbd.at))
            start(ST_WRITE);
        return;
    case ST_GAP:
        if (time_reached(kbd.at))
            start(ST_READ);
        return;
    case ST_WRITE:
    case ST_READ:
        break;
    }

    uint32_t raw = hw()->raw_intr_stat;
    if (raw & I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS) {
        // NACK: the keyboard isn't there or isn't up yet. Reading clr_tx_abrt
        // releases the command FIFO, which the abort flushed.
        (void)hw()->clr_tx_abrt;
        set_online(false);
        wait_until(ST_WAIT, RETRY_MS * 1000);
        return;
    }

    if (!(raw & I2C_IC_RAW_INTR_STAT_STOP_DET_BITS)) {
        if (absolute_time_diff_us(kbd.started, get_absolute_time()) >
            XFER_TIMEOUT_MS * 1000) {
            printf("[kbd] i2c stuck, resetting the bus\n");
            set_online(false);
            bus_reset();
            wait_until(ST_WAIT, RETRY_MS * 1000);
        }
        return;
    }

    if (kbd.state == ST_WRITE) {
        wait_until(ST_GAP, GAP_US);
        return;
    }

    if (hw()->rxflr < 2) { // STOP without our bytes; shouldn't happen
        wait_until(ST_WAIT, POLL_MS * 1000);
        return;
    }
    uint8_t state = (uint8_t)hw()->data_cmd;
    uint8_t code = (uint8_t)hw()->data_cmd;
    set_online(true);
    // Drain the FIFO quickly while it has entries, then idle-poll.
    wait_until(ST_WAIT, got_entry(state, code) ? 0 : POLL_MS * 1000);
}

bool kbd_get(kbd_event_t *ev) {
    if (!kbd.count)
        return false;
    *ev = kbd.queue[kbd.head];
    kbd.head = (uint8_t)((kbd.head + 1) % count_of(kbd.queue));
    kbd.count--;
    return true;
}

bool kbd_online(void) { return kbd.online; }
