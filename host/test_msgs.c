// Message store on its own, built with a tiny pool and DM table so eviction
// is easy to reach.
#include <stdio.h>
#include <string.h>

#include "msgs.h"
#include "nodedb.h"

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

static uint32_t next_id = 1;

static const mesh_msg_t *rx(mesh_conv_t conv, uint32_t from, const char *text) {
    mesh_msg_t m = {0};
    m.id = next_id++;
    m.from = from;
    m.to = conv.peer ? 1 : MESH_ADDR_BROADCAST;
    m.channel = conv.channel;
    m.status = MESH_MSG_RECEIVED;
    snprintf(m.text, sizeof m.text, "%s", text);
    return mesh_msgs_add(conv, &m);
}

static const char *text_at(mesh_conv_t c, int i) {
    const mesh_msg_t *m = mesh_msgs_recent(c, i);
    return m ? m->text : "(none)";
}

static int added, updated;
static void on_added(const mesh_msg_t *m, void *ctx) {
    (void)m;
    (void)ctx;
    added++;
}
static void on_updated(const mesh_msg_t *m, void *ctx) {
    (void)m;
    (void)ctx;
    updated++;
}

static void test_pool_eviction(void) {
    mesh_msgs_clear();
    const mesh_conv_t ch0 = MESH_CONV_CHANNEL(0), dm = MESH_CONV_DM(0x42);

    rx(dm, 0x42, "quiet dm");
    for (int i = 0; i < 8; i++) {
        char t[8];
        snprintf(t, sizeof t, "c%d", i);
        rx(ch0, 0x99, t);
    }
    // Pool of 6: the busy channel gave up its oldest; the DM survived.
    CHECK(mesh_msgs_count(dm) == 1);
    CHECK(mesh_msgs_count(ch0) == 5);
    CHECK(strcmp(text_at(ch0, 0), "c7") == 0);
    CHECK(strcmp(text_at(ch0, 4), "c3") == 0);
    CHECK(strcmp(text_at(dm, 0), "quiet dm") == 0);
    CHECK(mesh_msgs_conv(ch0)->unread == 8); // unread counts arrivals
    // seq keeps increasing across evictions.
    CHECK(mesh_msgs_recent(ch0, 0)->seq > mesh_msgs_recent(ch0, 1)->seq);
}

static void test_dm_eviction(void) {
    mesh_msgs_clear();
    rx(MESH_CONV_DM(0x10), 0x10, "a");
    rx(MESH_CONV_DM(0x20), 0x20, "b");
    rx(MESH_CONV_DM(0x10), 0x10, "a2"); // 0x10 is now the more recent
    rx(MESH_CONV_CHANNEL(3), 0x99, "ch");
    rx(MESH_CONV_DM(0x30), 0x30, "c"); // table full: 0x20 goes
    CHECK(mesh_msgs_conv(MESH_CONV_DM(0x20)) == NULL);
    CHECK(mesh_msgs_count(MESH_CONV_DM(0x10)) == 2);
    CHECK(mesh_msgs_count(MESH_CONV_DM(0x30)) == 1);
    CHECK(mesh_msgs_count(MESH_CONV_CHANNEL(3)) == 1);
    CHECK(mesh_msgs_conv_count() == 3);
    CHECK(mesh_msgs_conv_at(0)->conv.peer == 0x30);
    CHECK(mesh_msgs_conv_at(1)->conv.peer == 0 && mesh_msgs_conv_at(1)->conv.channel == 3);
    CHECK(mesh_msgs_conv_at(2)->conv.peer == 0x10);
    CHECK(mesh_msgs_conv_at(3) == NULL);
    // Channel index out of range is refused.
    CHECK(rx(MESH_CONV_CHANNEL(MESH_MAX_CHANNELS), 0x99, "x") == NULL);
}

static void test_status_and_observer(void) {
    mesh_msgs_clear();
    mesh_msgs_observer_t obs = {on_added, on_updated, NULL};
    mesh_msgs_set_observer(&obs);

    mesh_msg_t m = {0};
    m.id = 77;
    m.from = 1;
    m.to = 0x42;
    m.status = MESH_MSG_QUEUED;
    strcpy(m.text, "out");
    mesh_msg_t *s = mesh_msgs_add(MESH_CONV_DM(0x42), &m);
    CHECK(s && added == 1);
    CHECK(mesh_msgs_unread_total() == 0);
    CHECK(mesh_msgs_find_outgoing(77) == s);
    CHECK(mesh_msgs_find_outgoing(0) == NULL);

    mesh_msgs_set_status(s, MESH_MSG_DELIVERED, 0);
    mesh_msgs_set_status(s, MESH_MSG_RELAYED, 0); // no going back
    mesh_msgs_set_status(s, MESH_MSG_FAILED, 5);  // delivered is final
    CHECK(s->status == MESH_MSG_DELIVERED && updated == 1);

    // Restored messages: no unread, no observer call, but listed.
    mesh_msg_t old = {0};
    old.id = 5;
    old.from = 0x42;
    old.status = MESH_MSG_RECEIVED;
    old.local_ms = 1234;
    strcpy(old.text, "from the sd card");
    CHECK(mesh_msgs_restore(MESH_CONV_DM(0x42), &old) != NULL);
    CHECK(added == 1 && mesh_msgs_unread_total() == 0);
    CHECK(mesh_msgs_count(MESH_CONV_DM(0x42)) == 2);
    CHECK(mesh_msgs_recent(MESH_CONV_DM(0x42), 0)->local_ms == 1234);
    // Received message ids aren't outgoing ones.
    CHECK(mesh_msgs_find_outgoing(5) == NULL);
    mesh_msgs_set_observer(NULL);
}

static void test_text_bounds(void) {
    mesh_msgs_clear();
    mesh_msg_t m = {0};
    m.status = MESH_MSG_RECEIVED;
    memset(m.text, 'z', sizeof m.text); // no terminator
    const mesh_msg_t *s = mesh_msgs_add(MESH_CONV_CHANNEL(0), &m);
    CHECK(s && s->len == MESH_MSG_TEXT_MAX && s->text[MESH_MSG_TEXT_MAX] == '\0');
    // id 0 is never treated as a duplicate.
    CHECK(mesh_msgs_add(MESH_CONV_CHANNEL(0), &m) != NULL);
}

// The receive hook (message chimes): new incoming messages only, once each.
static int rx_dm, rx_ch;
static void on_rx(const mesh_msg_t *m) {
    if (MESH_CONV_IS_DM(mesh_msgs_conv_of(m)))
        rx_dm++;
    else
        rx_ch++;
}

static void test_rx_hook(void) {
    mesh_msgs_clear();
    mesh_msgs_set_rx_hook(on_rx);
    const mesh_msg_t *a = rx(MESH_CONV_DM(0x42), 0x42, "hi");
    rx(MESH_CONV_CHANNEL(1), 0x43, "cq");
    CHECK(rx_dm == 1 && rx_ch == 1);
    mesh_msg_t dup = *a; // heard again
    CHECK(mesh_msgs_add(MESH_CONV_DM(0x42), &dup) == NULL && rx_dm == 1);
    mesh_msg_t m = {0};
    m.id = 900;
    m.from = 1;
    m.to = 0x42;
    m.status = MESH_MSG_QUEUED; // ours
    CHECK(mesh_msgs_add(MESH_CONV_DM(0x42), &m) && rx_dm == 1);
    m.id = 901;
    m.from = 0x42;
    m.status = MESH_MSG_RECEIVED;
    CHECK(mesh_msgs_restore(MESH_CONV_DM(0x42), &m) && rx_dm == 1);
    mesh_msgs_set_rx_hook(NULL);
    rx(MESH_CONV_CHANNEL(1), 0x43, "unhooked");
    CHECK(rx_ch == 1);
}

int main(void) {
    mesh_db_clear();
    test_pool_eviction();
    test_dm_eviction();
    test_status_and_observer();
    test_text_bounds();
    test_rx_hook();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
