// Host-only stand-in for link.c: the UI is tested against the real message
// store (msgs.c) but no node, so the link's state, sending and clock are
// whatever the test sets.
#include <string.h>

#include "link.h"
#include "nodedb.h"
#include "scene.h"

mesh_link_state_t fake_link_state = MESH_LINK_READY;
mesh_send_result_t fake_send_result = MESH_SEND_OK;
uint32_t fake_epoch;

bool fake_node_silent;
mesh_link_stats_t fake_link_stats;
uint32_t fake_ms;
int fake_admin_sent;

const mesh_link_stats_t *mesh_link_stats(void) { return &fake_link_stats; }
uint32_t mesh_link_ms(void) { return fake_ms; }
void mesh_link_request_config(void) { fake_link_state = MESH_LINK_CONFIGURING; }

bool mesh_link_send_admin(const struct _meshtastic_AdminMessage *a) {
    (void)a;
    if (fake_link_state != MESH_LINK_READY)
        return false;
    fake_admin_sent++;
    return true;
}

mesh_link_state_t mesh_link_state(void) { return fake_link_state; }

mesh_link_conn_t mesh_link_conn(void) {
    switch (fake_link_state) {
    case MESH_LINK_READY: return MESH_CONN_UP;
    case MESH_LINK_PASSIVE: return MESH_CONN_DESKTOP;
    case MESH_LINK_IDLE: return MESH_CONN_NONE;
    default: return fake_node_silent ? MESH_CONN_NONE : MESH_CONN_SYNCING;
    }
}
uint32_t mesh_link_epoch(void) { return fake_epoch; }

// Stores the message as link.c does, without anything to send it to.
mesh_send_result_t mesh_link_send_text(mesh_conv_t conv, const char *text,
                                       size_t len, uint32_t reply_id,
                                       mesh_msg_t **out) {
    static uint32_t id = 0x7000;
    if (out)
        *out = NULL;
    if (fake_send_result != MESH_SEND_OK)
        return fake_send_result;
    if (!len)
        return MESH_SEND_EMPTY;
    if (len > MESH_MSG_SEND_MAX)
        return MESH_SEND_TOO_LONG;
    mesh_msg_t m = {0};
    m.id = ++id;
    m.from = mesh_db.my_node_num;
    m.to = conv.peer ? conv.peer : MESH_ADDR_BROADCAST;
    m.channel = conv.peer ? 0 : conv.channel;
    m.reply_id = reply_id;
    m.hops = 0xFF;
    m.status = MESH_MSG_QUEUED;
    memcpy(m.text, text, len);
    mesh_msg_t *stored = mesh_msgs_add(conv, &m);
    if (out)
        *out = stored;
    return stored ? MESH_SEND_OK : MESH_SEND_FAILED;
}

const char *mesh_link_send_result_name(mesh_send_result_t r) {
    static const char *const names[] = {
        "sent", "node not ready", "empty message", "too long",
        "no such channel", "couldn't send",
    };
    return (unsigned)r < 6 ? names[r] : "?";
}
