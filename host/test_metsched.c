// Scheduled metrics requests (src/mesh/metsched.c) against the stand-in link,
// and their log (src/sd/metlog.c) on a FAT-formatted RAM disk.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "metrics.h"
#include "metsched.h"
#include "nodedb.h"
#include "ramdisk.h"
#include "scene.h"
#include "sd/metlog.h"
#include "sd/settings.h"

static int failures;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
            failures++;                                                        \
        }                                                                      \
    } while (0)

#define NODE 0x0badf00du
#define MIN 60000u

static FATFS fs;

static void run_for(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 500) {
        fake_ms += 500;
        mesh_metrics_poll();
        metsched_task();
    }
}

static int sent_kind(void) {
    switch (fake_tel_variant) {
    case meshtastic_Telemetry_device_metrics_tag: return MET_DEVICE;
    case meshtastic_Telemetry_environment_metrics_tag: return MET_ENV;
    case meshtastic_Telemetry_power_metrics_tag: return MET_POWER;
    default: return MET_LOCAL;
    }
}

static void answer_local(uint32_t uptime, uint32_t rx, int32_t noise) {
    meshtastic_Telemetry t = meshtastic_Telemetry_init_zero;
    t.which_variant = meshtastic_Telemetry_local_stats_tag;
    t.variant.local_stats.uptime_seconds = uptime;
    t.variant.local_stats.num_packets_rx = rx;
    t.variant.local_stats.num_online_nodes = 12;
    t.variant.local_stats.noise_floor = noise;
    mesh_metrics_add(fake_tel_to, &t, fake_epoch);
    mesh_metrics_answer(fake_tel_id, fake_tel_to);
    fake_tel_id = 0;
}

static char *read_file(const char *path) {
    static char buf[4096];
    FIL f;
    UINT n = 0;
    if (f_open(&f, path, FA_READ) != FR_OK)
        return NULL;
    f_read(&f, buf, sizeof buf - 1, &n);
    f_close(&f);
    buf[n] = 0;
    return buf;
}

static int lines(const char *s) {
    int n = 0;
    for (; s && *s; s++)
        n += *s == '\n';
    return n;
}

static void setup(void) {
    static BYTE work[FF_MAX_SS];
    ramdisk_create(4u << 20);
    MKFS_PARM opt = {.fmt = FM_FAT | FM_SFD};
    CHECK(f_mkfs("", &opt, work, sizeof work) == FR_OK);
    CHECK(f_mount(&fs, "", 1) == FR_OK);
    metlog_set_online(true);

    mesh_db_clear();
    meshtastic_MyNodeInfo mi = meshtastic_MyNodeInfo_init_zero;
    mi.my_node_num = SCENE_ME;
    mesh_db_set_my_info(&mi);
    fake_link_state = MESH_LINK_READY;
    fake_ms = 1000;
    fake_epoch = SCENE_NOW;
    metsched_init();
}

static void test_kinds_and_steps(void) {
    char b[MET_KINDS + 1];
    uint8_t k;
    CHECK(strcmp(metsched_kinds_str(MET_BIT(MET_DEVICE) | MET_BIT(MET_LOCAL), b), "dl") == 0);
    CHECK(metsched_parse_kinds("LD", &k) && k == (MET_BIT(MET_DEVICE) | MET_BIT(MET_LOCAL)));
    CHECK(!metsched_parse_kinds("dx", &k));
    CHECK(!metsched_parse_kinds("", &k));
    CHECK(metsched_step(60, 1) == 120 && metsched_step(60, -1) == 30);
    CHECK(metsched_step(30, -1) == 30 && metsched_step(1440, 1) == 1440);
    CHECK(metsched_step(45, -1) == 30 && metsched_step(45, 1) == 120);
}

static void test_run(void) {
    // Too often is held to the minimum, and it's saved
    CHECK(metsched_set(NODE, MET_BIT(MET_DEVICE) | MET_BIT(MET_LOCAL), 5));
    const met_sched_t *s = metsched_find(NODE);
    CHECK(s && s->every_min == METSCHED_MIN_MIN);
    CHECK(settings_get("metrics.sched1") &&
          strcmp(settings_get("metrics.sched1"), "!0badf00d dl 30") == 0);

    // A new one runs at once: device metrics first
    int sent = fake_tel_sent;
    metsched_task();
    CHECK(fake_tel_sent == sent + 1 && fake_tel_to == NODE && sent_kind() == MET_DEVICE);
    met_kind_t k;
    int t;
    CHECK(metsched_running(NODE, &k, &t) && k == MET_DEVICE && t == 1);
    uint32_t started = fake_ms;
    CHECK(scene_telemetry_answer(0)); // device metrics; logged as an answer
    run_for(METSCHED_GAP_MS - 1000);
    CHECK(fake_tel_sent == sent + 1); // not before the gap
    run_for(1500); // the gap starts when the answer is seen
    CHECK(fake_tel_sent == sent + 2 && sent_kind() == MET_LOCAL);

    // Nothing comes back: asked once more five minutes later, then given up
    run_for(METSCHED_WAIT_MS);
    CHECK(metsched_running(NODE, &k, &t) && k == MET_KINDS);
    run_for(METSCHED_RETRY_MS - 1000);
    CHECK(fake_tel_sent == sent + 2);
    run_for(1500);
    CHECK(fake_tel_sent == sent + 3 && sent_kind() == MET_LOCAL);
    CHECK(metsched_running(NODE, &k, &t) && k == MET_LOCAL && t == 2);
    run_for(METSCHED_WAIT_MS + 1000);
    CHECK(fake_tel_sent == sent + 3);
    CHECK(!metsched_running(NODE, NULL, NULL));
    CHECK(s->last_ok == MET_BIT(MET_DEVICE) && s->last_failed == MET_BIT(MET_LOCAL));
    CHECK(s->last_error[MET_LOCAL] == MET_ERR_NO_ANSWER);
    CHECK(s->next_ms == started + 30 * MIN);

    // The log: the answer, and a line for what didn't come
    while (metlog_pending())
        CHECK(metlog_flush() == 0);
    const char *dev = read_file(METLOG_DIR "/0badf00d-device.tsv");
    CHECK(dev && strncmp(dev, "time\tgot\tbattery", 16) == 0 && lines(dev) == 2);
    CHECK(dev && strstr(dev, "\tanswer\t88\t13.050\t"));
    const char *loc = read_file(METLOG_DIR "/0badf00d-local.tsv");
    CHECK(loc && lines(loc) == 2 && strstr(loc, "\tno-answer\t-\t"));
    if (loc) {
        // as many columns as the header
        const char *nl = strchr(loc, '\n');
        int th = 0, tl = 0;
        for (const char *p = loc; p < nl; p++)
            th += *p == '\t';
        for (const char *p = nl + 1; *p; p++)
            tl += *p == '\t';
        CHECK(th == tl);
    }

    // Due again after the interval; a request made by hand meanwhile goes
    // first, and the run picks up after it
    run_for(30 * MIN - (fake_ms - started) - 1000);
    CHECK(fake_tel_sent == sent + 3);
    CHECK(mesh_metrics_request(SCENE_ME, MET_LOCAL));
    sent = fake_tel_sent;
    run_for(2000);
    CHECK(fake_tel_sent == sent); // ours is waiting
    answer_local(5000, 100, 0);
    run_for(METSCHED_GAP_MS);
    CHECK(fake_tel_sent == sent + 1 && fake_tel_to == NODE && sent_kind() == MET_DEVICE);
    // ... and if one replaces the run's, that kind is asked again after
    CHECK(mesh_metrics_request(SCENE_ME, MET_DEVICE));
    answer_local(5600, 120, 0); // (an answer of the wrong kind still answers)
    run_for(METSCHED_GAP_MS);
    CHECK(fake_tel_to == NODE && sent_kind() == MET_DEVICE);
    CHECK(scene_telemetry_answer(0));
    run_for(METSCHED_GAP_MS + 500);
    CHECK(fake_tel_to == NODE && sent_kind() == MET_LOCAL);
    fake_epoch += 1800;
    answer_local(7200, 1000, -110);
    run_for(METSCHED_GAP_MS + 500);
    CHECK(!metsched_running(NODE, NULL, NULL) && s->last_ok == s->kinds && !s->last_failed);

    // Two local stats far enough apart give a packet rate
    fake_epoch += 1800;
    fake_tel_to = NODE;
    answer_local(9000, 1300, -108);
    const mesh_metrics_t *e = mesh_metrics_find(NODE);
    CHECK(e && e->nhist >= 2);
    const met_point_t *p = mesh_metrics_point(e, e->nhist - 1);
    CHECK(p->rxph == 600 && p->noise == -108 && p->online == 12);

    // Telemetry heard unasked is logged as heard; our own node's, which
    // comes every minute or so, once per ten minutes
    while (metlog_pending())
        metlog_flush();
    int before = lines(read_file(METLOG_DIR "/0badf00d-local.tsv"));
    answer_local(9500, 1400, -108); // nobody's waiting: unasked
    fake_tel_to = SCENE_ME;
    CHECK(metsched_set(SCENE_ME, MET_BIT(MET_LOCAL), 60));
    for (int i = 0; i < 12; i++, fake_epoch += 60)
        answer_local(9500 + (uint32_t)i * 60, 1400, -108);
    while (metlog_pending())
        metlog_flush();
    const char *l2 = read_file(METLOG_DIR "/0badf00d-local.tsv");
    CHECK(lines(l2) == before + 1 && strstr(l2, "\theard\t9500\t"));
    const char *mine = read_file(METLOG_DIR "/a1b2c3d4-local.tsv");
    // (after the header, two answers to requests by hand from earlier)
    CHECK(mine && lines(mine) == 5 && strstr(mine, "\theard\t9500\t") &&
          strstr(mine, "\theard\t10100\t"));
    // ... and a node with no schedule is logged too
    fake_tel_to = 0x00c0ffee;
    answer_local(800, 10, -100);
    while (metlog_pending())
        metlog_flush();
    CHECK(read_file(METLOG_DIR "/00c0ffee-local.tsv") != NULL);
}

static void test_settings(void) {
    // A slot changed under it (a card loaded, say) is picked up; the running
    // state of nodes still there is kept
    const met_sched_t *s = metsched_find(NODE);
    uint32_t next = s->next_ms;
    CHECK(settings_set("metrics.sched1", "!0badf00d p 120m"));
    CHECK(settings_set("metrics.sched3", "!12345678 junk 60"));
    metsched_task();
    s = metsched_find(NODE);
    CHECK(s && s->kinds == MET_BIT(MET_POWER) && s->every_min == 120 && s->next_ms == next);
    CHECK(!metsched_find(0x12345678));
    CHECK(metsched_count() == 2);

    // Full up
    CHECK(metsched_set(1, MET_BIT(MET_DEVICE), 60));
    CHECK(metsched_set(2, MET_BIT(MET_DEVICE), 60));
    CHECK(!metsched_set(3, MET_BIT(MET_DEVICE), 60));
    // Removing one frees its slot
    CHECK(metsched_set(NODE, 0, 0));
    CHECK(!metsched_find(NODE) && metsched_count() == 3);
    CHECK(metsched_set(3, MET_BIT(MET_DEVICE), 60));
    CHECK(settings_get("metrics.sched4") && strcmp(settings_get("metrics.sched4"), "!00000003 d 60") == 0);
}

int main(void) {
    setup();
    test_kinds_and_steps();
    test_run();
    test_settings();
    if (failures)
        fprintf(stderr, "%d check(s) failed\n", failures);
    return failures != 0;
}
