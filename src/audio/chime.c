#include "audio/chime.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "pico/time.h"

#include "audio/morse.h"
#include "mesh/msgs.h"
#include "sd/settings.h"

#define PIN_L 26 // PWM 5A
#define PIN_R 27 // PWM 5B
#define TONE_HZ 700
#define WPM 20
#define WRAP 999 // counter top: duty in 0.1% steps
#define LEVEL ((WRAP + 1) / 2) // square wave

static const char *const texts[CHIME_KINDS] = {[CHIME_DM] = "RR", [CHIME_CHANNEL] = "C"};
static const char *const keys[CHIME_KINDS] = {[CHIME_DM] = SET_CHIME_DM,
                                              [CHIME_CHANNEL] = SET_CHIME_CHANNEL};

static uint slice;
static bool enabled[CHIME_KINDS];
static volatile bool pending[CHIME_KINDS];

// The one playing, walked by the alarm
static uint16_t steps[32];
static volatile int nsteps, step;
static volatile bool playing;
static uint32_t settings_seen = (uint32_t)-1;

static void tone(bool on) {
    pwm_set_both_levels(slice, on ? LEVEL : 0, on ? LEVEL : 0);
}

static int64_t next_step(alarm_id_t id, void *ctx) {
    (void)id, (void)ctx;
    if (++step >= nsteps) {
        tone(false);
        playing = false;
        return 0;
    }
    tone(!(step & 1)); // even steps sound
    return (int64_t)steps[step] * 1000;
}

static void on_received(const mesh_msg_t *m) {
    chime_request(MESH_CONV_IS_DM(mesh_msgs_conv_of(m)) ? CHIME_DM : CHIME_CHANNEL);
}

void chime_init(void) {
    slice = pwm_gpio_to_slice_num(PIN_L);
    pwm_config c = pwm_get_default_config();
    pwm_config_set_wrap(&c, WRAP);
    pwm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / ((float)TONE_HZ * (WRAP + 1)));
    pwm_init(slice, &c, true);
    tone(false);
    gpio_set_function(PIN_L, GPIO_FUNC_PWM);
    gpio_set_function(PIN_R, GPIO_FUNC_PWM);
    mesh_msgs_set_rx_hook(on_received);
    chime_task();
}

bool chime_enabled(chime_kind_t kind) { return enabled[kind]; }

void chime_request(chime_kind_t kind) {
    if (enabled[kind])
        pending[kind] = true;
}

void chime_play(chime_kind_t kind) { pending[kind] = true; }

void chime_task(void) {
    if (settings_version() != settings_seen) {
        settings_seen = settings_version();
        for (int k = 0; k < CHIME_KINDS; k++) {
            enabled[k] = settings_get_bool(keys[k], true);
            if (!enabled[k])
                pending[k] = false;
        }
    }
    if (playing)
        return;
    for (int k = 0; k < CHIME_KINDS; k++) {
        if (!pending[k])
            continue;
        pending[k] = false;
        nsteps = morse_timing(texts[k], WPM, steps, (int)(sizeof steps / sizeof *steps) - 1);
        if (!nsteps)
            continue;
        // A word gap after it, so the next chime doesn't run on
        steps[nsteps++] = 7 * 1200 / WPM;
        step = 0;
        playing = true;
        tone(true);
        if (add_alarm_in_ms(steps[0], next_step, NULL, true) < 0) {
            tone(false); // no alarm free; skip it
            playing = false;
        }
        return;
    }
}
