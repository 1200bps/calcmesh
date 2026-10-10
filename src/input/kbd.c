// Keyboard polling. Each poll is two I2C transactions, a register write
// (0x09, the FIFO) and a 2-byte read of [state, key]. At 10 kHz that's ~5 ms
// of bus time, so instead of the SDK's blocking calls we queue the bytes in
// the I2C block's own command FIFO and check back on later kbd_task() calls.
//
// Every few seconds a poll goes to the battery and backlight registers
// instead, and a backlight change goes out as the next poll. Those answer
// [register, value]; a write is the register with bit 7 set plus the value,
// and the reply has the value as the keyboard stored it (picocalc_keyboard's
// reg.h and receiveEvent()).

#include "input/kbd.h"

#include <stdio.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/time.h"

#include "input/kbd_map.h"

#define REG_LCD_BL 0x05 // screen backlight PWM
#define REG_FIFO 0x09
#define REG_KEY_BL 0x0A // keyboard backlight PWM
#define REG_BATTERY 0x0B // percent, bit 7 = charging
#define REG_WRITE 0x80

#define POLL_MS 10          // the keyboard scans its matrix every 16 ms
#define GAP_US 1000         // after the register write, before the read
#define XFER_TIMEOUT_MS 50  // a transaction stuck longer than this resets the bus
#define RETRY_MS 500        // after a NACK (keyboard absent or still booting)
#define POWER_MS 5000       // re-read battery and backlights this often

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
    // The transaction in flight: register, and for a write, the value.
    uint8_t reg;
    bool write;
    uint8_t value;
    // Values last read back (-1 = not yet), and writes waiting to go out.
    int16_t lcd_bl, key_bl, battery;
    int16_t lcd_bl_want, key_bl_want;
    absolute_time_t power_at;
    uint8_t power_reads; // registers still to read this round, bits from power_regs
    kbd_decoder_t dec;
    kbd_event_t queue[16];
    uint8_t head, count;
    bool online;
    bool ever_online;
} kbd = {
    .lcd_bl = -1, .key_bl = -1, .battery = -1,
    .lcd_bl_want = -1, .key_bl_want = -1,
};

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

static const uint8_t power_regs[] = {REG_BATTERY, REG_LCD_BL, REG_KEY_BL};

static int16_t *reg_value(uint8_t reg) {
    switch (reg) {
    case REG_LCD_BL: return &kbd.lcd_bl;
    case REG_KEY_BL: return &kbd.key_bl;
    case REG_BATTERY: return &kbd.battery;
    default: return NULL;
    }
}

static int16_t *reg_want(uint8_t reg) {
    switch (reg) {
    case REG_LCD_BL: return &kbd.lcd_bl_want;
    case REG_KEY_BL: return &kbd.key_bl_want;
    default: return NULL;
    }
}

// What the next poll should be: a queued backlight write, then a round of
// battery and backlight reads when one is due, otherwise the key FIFO.
static void pick_next(void) {
    kbd.write = false;
    kbd.reg = REG_FIFO;
    if (kbd.lcd_bl_want >= 0 || kbd.key_bl_want >= 0) {
        kbd.reg = kbd.lcd_bl_want >= 0 ? REG_LCD_BL : REG_KEY_BL;
        int16_t *want = reg_want(kbd.reg);
        kbd.write = true;
        kbd.value = (uint8_t)*want;
        *want = -1;
        return;
    }
    if (!kbd.power_reads && time_reached(kbd.power_at)) {
        kbd.power_reads = (1u << count_of(power_regs)) - 1;
        kbd.power_at = make_timeout_time_ms(POWER_MS);
    }
    for (unsigned i = 0; i < count_of(power_regs); i++)
        if (kbd.power_reads & (1u << i)) {
            kbd.power_reads &= (uint8_t)~(1u << i);
            kbd.reg = power_regs[i];
            return;
        }
}

// The poll in flight didn't happen: put a write back so it goes out once the
// keyboard answers again, unless a newer one has been queued since.
static void requeue(void) {
    int16_t *want = kbd.write ? reg_want(kbd.reg) : NULL;
    if (want && *want < 0)
        *want = kbd.value;
    kbd.write = false;
}

static void start(state_t next) {
    // Clear what the last transaction left behind.
    (void)hw()->clr_stop_det;
    while (hw()->rxflr)
        (void)hw()->data_cmd;

    if (next == ST_WRITE) {
        pick_next();
        if (kbd.write) {
            hw()->data_cmd = kbd.reg | REG_WRITE;
            hw()->data_cmd = kbd.value | I2C_IC_DATA_CMD_STOP_BITS;
        } else {
            hw()->data_cmd = kbd.reg | I2C_IC_DATA_CMD_STOP_BITS;
        }
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
    kbd.power_at = get_absolute_time();
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
        requeue();
        set_online(false);
        wait_until(ST_WAIT, RETRY_MS * 1000);
        return;
    }

    if (!(raw & I2C_IC_RAW_INTR_STAT_STOP_DET_BITS)) {
        if (absolute_time_diff_us(kbd.started, get_absolute_time()) >
            XFER_TIMEOUT_MS * 1000) {
            printf("[kbd] i2c stuck, resetting the bus\n");
            requeue();
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
        requeue();
        wait_until(ST_WAIT, POLL_MS * 1000);
        return;
    }
    uint8_t state = (uint8_t)hw()->data_cmd;
    uint8_t code = (uint8_t)hw()->data_cmd;
    set_online(true);
    if (kbd.reg != REG_FIFO) {
        // [register, value]; anything else means the keyboard wasn't
        // answering what we asked, so leave the old value.
        int16_t *v = reg_value(kbd.reg);
        if (v && state == kbd.reg)
            *v = code;
        else
            requeue();
        kbd.write = false;
        wait_until(ST_WAIT, 0);
        return;
    }
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

void kbd_set_lcd_backlight(uint8_t level) { kbd.lcd_bl_want = level; }
int kbd_lcd_backlight(void) { return kbd.lcd_bl; }
void kbd_set_key_backlight(uint8_t level) { kbd.key_bl_want = level; }
int kbd_key_backlight(void) { return kbd.key_bl; }

int kbd_battery(bool *charging) {
    if (charging)
        *charging = kbd.battery >= 0 && (kbd.battery & 0x80);
    return kbd.battery < 0 ? -1 : (kbd.battery & 0x7F);
}
