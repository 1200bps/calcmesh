// DEBUG ONLY (not for merging): the USB stall hunt. Every 2 s, a sample of
// USB controller and TinyUSB state goes into a RAM ring (and, with a card, a
// line to /calcmesh/usbdiag.tsv). When the console's transmit FIFO hasn't
// drained for STALL_MS while the host has the port open, the ring is frozen
// and USB is disconnected and reconnected (like a replug), so `usbdiag`
// can print what led up to it.

#include "usbdiag.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "hardware/irq.h"
#include "hardware/structs/usb.h"
#include "pico/stdlib.h"
#include "sd/sd.h"
#include "tusb.h"

#define PATH "/calcmesh/usbdiag.tsv"
#define EVERY_MS 2000
#define MAX_BYTES (2u * 1024 * 1024)
#define RING 48         // 96 s of samples
#define STALL_MS 15000  // console FIFO stuck this long: a stall
#define RECONNECT_MS 1000

typedef struct {
    uint32_t ms, loops;
    uint8_t mounted, suspended, c0conn, c1conn, c2conn, nvic_en;
    uint16_t c0wav, c0rav, c2wav, sof;
    uint32_t sie_status, sie_ctrl, inte, ints, buf_status;
    uint32_t ep[6][2]; // buffer control, endpoints 1-6, IN and OUT
} sample_t;

static sample_t ring[RING], frozen[RING];
static int ring_head, ring_count, frozen_count;
static uint32_t frozen_ms, stalls, loops, next_ms;
static bool file_started;
static FIL fil;

// Stall detector
static uint32_t c0wav_last, c0wav_since;
static uint32_t reconnect_at; // 0: not disconnected

void usbdiag_loop(void) { loops++; }

static void take(sample_t *s, uint32_t now) {
    s->ms = now;
    s->loops = loops;
    s->mounted = tud_mounted();
    s->suspended = tud_suspended();
    s->c0conn = tud_cdc_n_connected(0);
    s->c1conn = tud_cdc_n_connected(1);
    s->c2conn = tud_cdc_n_connected(2);
    s->c0wav = (uint16_t)tud_cdc_n_write_available(0);
    s->c0rav = (uint16_t)tud_cdc_n_available(0);
    s->c2wav = (uint16_t)tud_cdc_n_write_available(2);
    s->sof = (uint16_t)(usb_hw->sof_rd & 0x7ff);
    s->sie_status = usb_hw->sie_status;
    s->sie_ctrl = usb_hw->sie_ctrl;
    s->inte = usb_hw->inte;
    s->ints = usb_hw->ints;
    s->buf_status = usb_hw->buf_status;
    s->nvic_en = irq_is_enabled(USBCTRL_IRQ);
    for (int i = 0; i < 6; i++) {
        s->ep[i][0] = usb_dpram->ep_buf_ctrl[i + 1].in;
        s->ep[i][1] = usb_dpram->ep_buf_ctrl[i + 1].out;
    }
}

static int format(const sample_t *s, char *p, size_t size) {
    int n = snprintf(p, size,
                     "%lu\t%lu\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%08lx\t%08lx\t%08lx\t%08lx"
                     "\t%08lx\t%u",
                     (unsigned long)s->ms, (unsigned long)s->loops, s->mounted, s->suspended,
                     s->c0conn, s->c0wav, s->c0rav, s->c1conn, s->c2conn, s->c2wav, s->sof,
                     (unsigned long)s->sie_status, (unsigned long)s->sie_ctrl,
                     (unsigned long)s->inte, (unsigned long)s->ints,
                     (unsigned long)s->buf_status, s->nvic_en);
    for (int i = 0; i < 6 && (size_t)n < size; i++)
        n += snprintf(p + n, size - (size_t)n, "\t%08lx\t%08lx", (unsigned long)s->ep[i][0],
                      (unsigned long)s->ep[i][1]);
    if ((size_t)n < size - 1)
        p[n++] = '\n';
    return n;
}

static const char header[] =
    "# ms\tloops\tmounted\tsuspended\tc0conn\tc0wav\tc0rav\tc1conn\tc2conn\tc2wav\tsof"
    "\tsie_status\tsie_ctrl\tinte\tints\tbuf_status\tnvic_en\tep1in\tep1out\tep2in\tep2out"
    "\tep3in\tep3out\tep4in\tep4out\tep5in\tep5out\tep6in\tep6out\n";

static void to_card(const sample_t *s, const char *note) {
    if (sd_state() != SD_READY)
        return;
    char ln[400];
    int n = 0;
    if (!file_started) {
        file_started = true;
        n = snprintf(ln, sizeof ln, "# boot\n%s", header);
    }
    if (note)
        n += snprintf(ln + n, sizeof ln - (size_t)n, "# %s\n", note);
    n += format(s, ln + n, sizeof ln - (size_t)n);
    if (f_open(&fil, PATH, FA_OPEN_APPEND | FA_WRITE) != FR_OK)
        return;
    UINT put;
    if (f_size(&fil) < MAX_BYTES)
        f_write(&fil, ln, (UINT)n, &put);
    f_close(&fil);
}

static void freeze(uint32_t now) {
    for (int i = 0; i < ring_count; i++)
        frozen[i] = ring[(ring_head - ring_count + i + RING) % RING];
    frozen_count = ring_count;
    frozen_ms = now;
    stalls++;
}

void usbdiag_task(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (reconnect_at) {
        if ((int32_t)(now - reconnect_at) >= 0) {
            reconnect_at = 0;
            c0wav_since = now;
            tud_connect();
        }
        return;
    }

    // Stall: the host has the console open but nothing leaves the FIFO
    uint32_t wav = tud_cdc_n_write_available(0);
    if (!tud_cdc_n_connected(0) || wav >= CFG_TUD_CDC_TX_BUFSIZE || wav != c0wav_last) {
        c0wav_last = wav;
        c0wav_since = now;
    } else if (now - c0wav_since >= STALL_MS) {
        sample_t s;
        take(&s, now);
        ring[ring_head] = s;
        ring_head = (ring_head + 1) % RING;
        if (ring_count < RING)
            ring_count++;
        freeze(now);
        to_card(&s, "stall: console FIFO stuck; reconnecting");
        tud_disconnect();
        reconnect_at = now + RECONNECT_MS;
        c0wav_last = 0;
        return;
    }

    if ((int32_t)(now - next_ms) < 0)
        return;
    next_ms = now + EVERY_MS;
    sample_t *s = &ring[ring_head];
    take(s, now);
    ring_head = (ring_head + 1) % RING;
    if (ring_count < RING)
        ring_count++;
    loops = 0;
    to_card(s, NULL);
}

void usbdiag_print(int lines) {
    if (lines <= 0 || lines > RING)
        lines = RING;
    char ln[400];
    printf("usbdiag: %lu stalls seen; now %lu ms\n", (unsigned long)stalls,
           (unsigned long)to_ms_since_boot(get_absolute_time()));
    if (frozen_count) {
        printf("== frozen at the last stall (%lu ms), oldest first\n", (unsigned long)frozen_ms);
        fputs(header, stdout);
        for (int i = frozen_count > lines ? frozen_count - lines : 0; i < frozen_count; i++) {
            format(&frozen[i], ln, sizeof ln);
            fwrite(ln, 1, strcspn(ln, "\n") + 1, stdout);
            stdio_flush();
        }
    }
    int recent = ring_count < 8 ? ring_count : 8;
    printf("== latest %d samples\n", recent);
    for (int i = ring_count - recent; i < ring_count; i++) {
        format(&ring[(ring_head - ring_count + i + RING) % RING], ln, sizeof ln);
        fwrite(ln, 1, strcspn(ln, "\n") + 1, stdout);
        stdio_flush();
    }
}
