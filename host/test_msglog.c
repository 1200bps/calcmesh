// SD card message history (src/sd/msglog.c) on a FAT-formatted RAM disk,
// with the store built small (6 messages, 2 DMs) so restore has to choose.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "meshtastic/mesh.pb.h"
#include "mesh/msgs.h"
#include "ramdisk.h"
#include "sd/msglog.h"

#if MESH_MSG_POOL != 6 || MESH_MAX_DMS != 2
#error "build with -DMESH_MSG_POOL=6 -DMESH_MAX_DMS=2"
#endif

static int failures;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
            failures++;                                                        \
        }                                                                      \
    } while (0)

#define ME 0x1200b0b5u

static FATFS fs;
static uint32_t next_id = 0x100;

// Cards over 32 GB come formatted exFAT; the rest FAT.
static BYTE card_fmt = FM_FAT;

static void fresh_card(void) {
    static BYTE work[FF_MAX_SS];
    ramdisk_create(8u << 20);
    MKFS_PARM opt = {.fmt = card_fmt | FM_SFD};
    CHECK(f_mkfs("", &opt, work, sizeof work) == FR_OK);
    CHECK(f_mount(&fs, "", 1) == FR_OK);
}

// Power cycle: the store forgets everything, the card is remounted.
static int reboot(void) {
    mesh_msgs_set_observer(NULL);
    msglog_close();
    mesh_msgs_clear();
    CHECK(f_mount(NULL, "", 0) == FR_OK);
    CHECK(f_mount(&fs, "", 1) == FR_OK);
    return msglog_open(true);
}

static void flush_all(void) {
    int guard = 0;
    while (msglog_pending() && guard++ < 1000)
        CHECK(msglog_flush() == 1);
}

static mesh_msg_t *rx(mesh_conv_t conv, uint32_t from, const char *text) {
    mesh_msg_t m = {0};
    m.id = next_id++;
    m.from = from;
    m.to = conv.peer ? ME : MESH_ADDR_BROADCAST;
    m.channel = conv.channel;
    m.status = MESH_MSG_RECEIVED;
    m.rx_time = 1791585668; // 2026-10-09T22:41:08Z
    m.snr = -7.25f;
    m.rssi = -118;
    m.hops = 2;
    snprintf(m.text, sizeof m.text, "%s", text);
    return mesh_msgs_add(conv, &m);
}

static mesh_msg_t *tx(mesh_conv_t conv, const char *text) {
    mesh_msg_t m = {0};
    m.id = next_id++;
    m.from = ME;
    m.to = conv.peer ? conv.peer : MESH_ADDR_BROADCAST;
    m.channel = conv.channel;
    m.status = MESH_MSG_QUEUED;
    m.hops = 0xFF;
    snprintf(m.text, sizeof m.text, "%s", text);
    return mesh_msgs_add(conv, &m);
}

// Whole file into a malloc'd string (NULL if missing).
static char *slurp(const char *path) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK)
        return NULL;
    UINT n = (UINT)f_size(&f), got;
    char *buf = malloc(n + 1);
    CHECK(f_read(&f, buf, n, &got) == FR_OK && got == n);
    buf[got] = '\0';
    f_close(&f);
    return buf;
}

static int count_lines(const char *s) {
    int n = 0;
    for (; *s; s++)
        n += *s == '\n';
    return n;
}

static void append_raw(const char *path, const char *bytes) {
    FIL f;
    UINT put;
    CHECK(f_open(&f, path, FA_OPEN_APPEND | FA_WRITE) == FR_OK);
    CHECK(f_write(&f, bytes, (UINT)strlen(bytes), &put) == FR_OK);
    f_close(&f);
}

// Record number on the file's last line.
static unsigned long last_n(const char *path) {
    char *f = slurp(path);
    if (!f)
        return 0;
    const char *ln = f + strlen(f) - 1;
    while (ln > f && ln[-1] != '\n')
        ln--;
    unsigned long n = strtoul(ln, NULL, 10);
    free(f);
    return n;
}

static const char *text_at(mesh_conv_t c, int i) {
    const mesh_msg_t *m = mesh_msgs_recent(c, i);
    return m ? m->text : "(none)";
}

static void test_format_roundtrip(void) {
    mesh_msg_t m = {0}, back;
    m.id = 0xdeadbeef;
    m.from = 0xa1b2c3d4;
    m.to = MESH_ADDR_BROADCAST;
    m.channel = 3;
    m.status = MESH_MSG_RECEIVED;
    m.rx_time = 1791585668;
    m.reply_id = 0x1234;
    m.snr = 6.5f;
    m.rssi = -97;
    m.hops = 0xFF;
    m.emoji = true;
    m.via_mqtt = true;
    snprintf(m.text, sizeof m.text, "tab\there\nnew\\line \xe2\x9c\x93");
    char buf[MSGLOG_LINE_MAX + 1];
    size_t so;
    size_t len = msglog_format(&m, 42, buf, &so);
    const char *want = "42\t2026-10-09T22:41:08Z\t!a1b2c3d4\t!ffffffff\t3\tdeadbeef\t"
                       "00001234\treceived \t000\t6.50\t-97\t-\tem\t"
                       "tab\\there\\nnew\\\\line \xe2\x9c\x93\n";
    CHECK(strcmp(buf, want) == 0);
    if (strcmp(buf, want))
        fprintf(stderr, "  got  %s  want %s", buf, want);
    CHECK(len == strlen(want));
    CHECK(strncmp(buf + so, "received ", 9) == 0);

    uint32_t n;
    size_t so2;
    CHECK(msglog_parse(buf, &back, &n, &so2));
    CHECK(n == 42 && so2 == so);
    CHECK(back.id == m.id && back.from == m.from && back.to == m.to);
    CHECK(back.channel == 3 && back.status == MESH_MSG_RECEIVED);
    CHECK(back.rx_time == m.rx_time && back.reply_id == m.reply_id);
    CHECK(back.snr == 6.5f && back.rssi == -97 && back.hops == 0xFF);
    CHECK(back.emoji && back.via_mqtt);
    CHECK(strcmp(back.text, m.text) == 0 && back.len == strlen(m.text));

    // Outgoing, failed, no time.
    mesh_msg_t o = {0};
    o.id = 7;
    o.from = ME;
    o.to = 0x42;
    o.status = MESH_MSG_FAILED;
    o.error = meshtastic_Routing_Error_NO_ROUTE;
    snprintf(o.text, sizeof o.text, "hi");
    msglog_format(&o, 1, buf, &so);
    CHECK(strcmp(buf, "1\t-\t!1200b0b5\t!00000042\t0\t00000007\t00000000\tfailed   \t001"
                      "\t-\t-\t-\t-\thi\n") == 0);
    CHECK(msglog_parse(buf, &back, &n, &so2));
    CHECK(back.status == MESH_MSG_FAILED && back.error == 1 && back.rx_time == 0);
    CHECK(back.hops == 0xFF && !back.emoji && !back.via_mqtt);

    // The longest text, all escapes, still fits.
    memset(o.text, '\n', MESH_MSG_TEXT_MAX);
    o.text[MESH_MSG_TEXT_MAX] = '\0';
    o.rx_time = 0xFFFFFFFF;
    len = msglog_format(&o, 0xFFFFFFFF, buf, &so);
    CHECK(len <= MSGLOG_LINE_MAX);
    CHECK(msglog_parse(buf, &back, &n, &so2) && back.len == MESH_MSG_TEXT_MAX);

    char hdr[] = "n\ttime\tfrom\tto\tch\tid\treply\tstatus\terr\tsnr\trssi\thops\tflags\ttext\n";
    CHECK(!msglog_parse(hdr, &back, &n, &so2));
    char torn[] = "12\t2026-10-09T22:41:08Z\t!a1b2c3d4\t!ffff";
    CHECK(!msglog_parse(torn, &back, &n, &so2));
    char bad_ch[] = "1\t-\t!00000001\t!ffffffff\t9\t00000001\t00000000\treceived \t000\t0\t0\t-\t-\tx";
    CHECK(!msglog_parse(bad_ch, &back, &n, &so2));
}

static void test_fattime(void) {
    // 2026-10-09 22:41:08 -> FAT: year 46, month 10, day 9, 22:41:08/2.
    CHECK(msglog_fattime(1791585668) ==
          (46u << 25 | 10u << 21 | 9u << 16 | 22u << 11 | 41u << 5 | 4u));
    CHECK(msglog_fattime(0) == (46u << 25 | 1u << 21 | 1u << 16));
}

static void test_log_and_restore(void) {
    fresh_card();
    mesh_msgs_set_observer(NULL);
    mesh_msgs_clear();
    CHECK(msglog_open(true) == 0); // empty card: makes the directories
    FILINFO fi;
    CHECK(f_stat(MSGLOG_DIR, &fi) == FR_OK && (fi.fattrib & AM_DIR));

    const mesh_conv_t ch0 = MESH_CONV_CHANNEL(0), dm = MESH_CONV_DM(0x42);
    rx(ch0, 0xa1, "morning all");
    mesh_msg_t *a = tx(ch0, "hello mesh");
    mesh_msg_t *b = tx(dm, "you up?");
    mesh_msg_t *c = tx(dm, "still sending");
    rx(dm, 0x42, "yes");
    // Status changes queued behind their own lines.
    mesh_msgs_set_status(a, MESH_MSG_SENT, 0);
    mesh_msgs_set_status(a, MESH_MSG_RELAYED, 0);
    mesh_msgs_set_status(b, MESH_MSG_SENT, 0);
    (void)c;
    flush_all();
    mesh_msgs_set_status(b, MESH_MSG_DELIVERED, 0);
    flush_all();
    CHECK(msglog_stats()->dropped == 0);

    char *f0 = slurp(MSGLOG_DIR "/ch0.tsv");
    char *fd = slurp(MSGLOG_DIR "/dm-00000042.tsv");
    CHECK(f0 && fd);
    if (f0 && fd) {
        CHECK(strncmp(f0, "n\ttime\t", 7) == 0);
        CHECK(count_lines(f0) == 3 && count_lines(fd) == 4);
        CHECK(strstr(f0, "\trelayed  \t000\t-\t-\t-\t-\thello mesh\n"));
        CHECK(strstr(f0, "\treceived \t000\t-7.25\t-118\t2\t-\tmorning all\n"));
        CHECK(strstr(fd, "\tdelivered\t000\t-\t-\t-\t-\tyou up?\n"));
        CHECK(strstr(fd, "\tsending  \t000\t-\t-\t-\t-\tstill sending\n"));
    }
    free(f0);
    free(fd);

    // Reboot: everything comes back, the DM (newest) on top, unread 0.
    CHECK(reboot() == 5);
    CHECK(msglog_stats()->restored == 5);
    CHECK(mesh_msgs_conv_count() == 2);
    CHECK(mesh_msgs_conv_at(0)->conv.peer == 0x42);
    CHECK(mesh_msgs_conv_at(1)->conv.peer == 0);
    CHECK(mesh_msgs_unread_total() == 0);
    CHECK(mesh_msgs_count(dm) == 3 && mesh_msgs_count(ch0) == 2);
    CHECK(strcmp(text_at(dm, 0), "yes") == 0);
    CHECK(strcmp(text_at(dm, 2), "you up?") == 0);
    CHECK(mesh_msgs_recent(dm, 2)->status == MESH_MSG_DELIVERED);
    CHECK(mesh_msgs_recent(ch0, 0)->status == MESH_MSG_RELAYED);
    const mesh_msg_t *r = mesh_msgs_recent(ch0, 1);
    CHECK(r->from == 0xa1 && r->snr == -7.25f && r->rssi == -118 && r->hops == 2);
    // The one in flight at power-off can never be acked now.
    const mesh_msg_t *lost = mesh_msgs_recent(dm, 1);
    CHECK(lost->status == MESH_MSG_FAILED &&
          lost->error == meshtastic_Routing_Error_TIMEOUT);
    fd = slurp(MSGLOG_DIR "/dm-00000042.tsv");
    CHECK(fd && strstr(fd, "\tfailed   \t003\t-\t-\t-\t-\tstill sending\n"));
    free(fd);

    // Logging carries on with later record numbers, and restored messages
    // aren't written twice.
    unsigned long dm_n = last_n(MSGLOG_DIR "/dm-00000042.tsv");
    rx(ch0, 0xa1, "after reboot");
    flush_all();
    f0 = slurp(MSGLOG_DIR "/ch0.tsv");
    CHECK(f0 && count_lines(f0) == 4 && strstr(f0, "\tafter reboot\n"));
    free(f0);
    CHECK(last_n(MSGLOG_DIR "/ch0.tsv") == dm_n + 1);
}

static void test_restore_keeps_newest(void) {
    fresh_card();
    mesh_msgs_set_observer(NULL);
    mesh_msgs_clear();
    CHECK(msglog_open(true) == 0);

    // Four DMs, oldest first; the store holds two.
    for (uint32_t p = 1; p <= 4; p++) {
        char t[16];
        snprintf(t, sizeof t, "from %lu", (unsigned long)p);
        rx(MESH_CONV_DM(p), p, t);
        flush_all();
    }
    // Ten messages on channel 1: only the newest six can come back.
    for (int i = 0; i < 10; i++) {
        char t[16];
        snprintf(t, sizeof t, "c%d", i);
        rx(MESH_CONV_CHANNEL(1), 0xa1, t);
    }
    flush_all();
    // Then DM 1 again, so it's the most recent DM.
    rx(MESH_CONV_DM(1), 1, "1 again");
    flush_all();

    reboot();
    CHECK(mesh_msgs_conv_count() == 3);
    CHECK(mesh_msgs_conv_at(0)->conv.peer == 1);
    CHECK(mesh_msgs_conv(MESH_CONV_DM(4)) != NULL);
    CHECK(mesh_msgs_conv(MESH_CONV_DM(2)) == NULL);
    CHECK(mesh_msgs_conv(MESH_CONV_DM(3)) == NULL);
    CHECK(strcmp(text_at(MESH_CONV_DM(1), 0), "1 again") == 0);
    CHECK(strcmp(text_at(MESH_CONV_CHANNEL(1), 0), "c9") == 0);
    int total = 0;
    for (int i = 0; i < mesh_msgs_conv_count(); i++)
        total += mesh_msgs_conv_at(i)->count;
    CHECK(total == MESH_MSG_POOL);
}

static void test_torn_line(void) {
    fresh_card();
    mesh_msgs_set_observer(NULL);
    mesh_msgs_clear();
    CHECK(msglog_open(true) == 0);
    rx(MESH_CONV_CHANNEL(0), 0xa1, "one");
    flush_all();
    // Power lost halfway through the next line.
    append_raw(MSGLOG_DIR "/ch0.tsv", "2\t2026-10-09T22:4");
    CHECK(reboot() == 1);
    rx(MESH_CONV_CHANNEL(0), 0xa1, "two");
    flush_all();
    CHECK(reboot() == 2);
    CHECK(strcmp(text_at(MESH_CONV_CHANNEL(0), 0), "two") == 0);
    CHECK(strcmp(text_at(MESH_CONV_CHANNEL(0), 1), "one") == 0);
    // Strangers in the directory are left alone.
    append_raw(MSGLOG_DIR "/notes.txt", "hi\n");
    append_raw(MSGLOG_DIR "/ch99.tsv", "1\t-\n");
    CHECK(reboot() == 2);
}

static void test_offline(void) {
    fresh_card();
    mesh_msgs_set_observer(NULL);
    mesh_msgs_clear();
    CHECK(msglog_open(true) == 0);
    uint32_t dropped = msglog_stats()->dropped;

    // Card pulled with records queued: they're dropped, not written later.
    rx(MESH_CONV_CHANNEL(0), 0xa1, "queued");
    msglog_close();
    CHECK(msglog_stats()->dropped == dropped + 1);
    rx(MESH_CONV_CHANNEL(0), 0xa1, "while out");
    CHECK(!msglog_pending() && msglog_flush() == 0);
    CHECK(msglog_stats()->dropped == dropped + 2);

    // Back in, without a restore: only what comes next is logged.
    CHECK(msglog_open(false) == 0);
    mesh_msg_t *m = tx(MESH_CONV_CHANNEL(0), "back");
    flush_all();
    // A status change for a message whose line was never written is skipped.
    mesh_msg_t *lost = (mesh_msg_t *)mesh_msgs_recent(MESH_CONV_CHANNEL(0), 1);
    lost->status = MESH_MSG_QUEUED; // pretend it was ours
    mesh_msgs_set_status(lost, MESH_MSG_SENT, 0);
    mesh_msgs_set_status(m, MESH_MSG_SENT, 0);
    flush_all();
    char *f0 = slurp(MSGLOG_DIR "/ch0.tsv");
    CHECK(f0 && count_lines(f0) == 2 && strstr(f0, "\tsent     \t000\t-\t-\t-\t-\tback\n"));
    free(f0);

    // A write error takes it offline.
    rx(MESH_CONV_CHANNEL(0), 0xa1, "doomed");
    ramdisk_fail(0);
    CHECK(msglog_flush() == -1);
    CHECK(!msglog_online());
    CHECK(msglog_stats()->errors == 1);
    ramdisk_fail(-1);
}

static void test_queue_full(void) {
    fresh_card();
    mesh_msgs_set_observer(NULL);
    mesh_msgs_clear();
    CHECK(msglog_open(true) == 0);
    uint32_t dropped = msglog_stats()->dropped;
    char big[MESH_MSG_TEXT_MAX + 1];
    memset(big, 'x', MESH_MSG_TEXT_MAX);
    big[MESH_MSG_TEXT_MAX] = '\0';
    for (int i = 0; i < 40; i++)
        rx(MESH_CONV_CHANNEL(0), 0xa1, big);
    CHECK(msglog_stats()->dropped > dropped);
    uint32_t lost = msglog_stats()->dropped - dropped;
    flush_all();
    char *f0 = slurp(MSGLOG_DIR "/ch0.tsv");
    CHECK(f0 && (uint32_t)count_lines(f0) == 1 + 40 - lost);
    free(f0);
}

int main(void) {
    test_format_roundtrip();
    test_fattime();
    test_log_and_restore();
    card_fmt = FM_EXFAT;
    test_log_and_restore();
    card_fmt = FM_FAT;
    test_restore_keeps_newest();
    test_torn_line();
    test_offline();
    test_queue_full();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("msglog: all tests passed\n");
    return 0;
}
