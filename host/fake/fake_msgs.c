// Host-only stand-in for the message store (see msgs.h here). Unbounded by
// the real pool sizes; it only has to be right, not small.
#include <stdio.h>
#include <string.h>

#include "msgs.h"
#include "nodedb.h"

#define MAX_MSGS 512
#define MAX_CONVS 128

static mesh_msg_t msgs[MAX_MSGS];
static bool read_flag[MAX_MSGS];
static int count;
static uint32_t version, seq;
static mesh_conv_info_t convs[MAX_CONVS];
static int nconvs;
mesh_send_result_t fake_send_result = MESH_SEND_OK;
uint32_t fake_epoch;

static bool same(mesh_conv_t a, mesh_conv_t b) {
    return a.peer == b.peer && (a.peer || a.channel == b.channel);
}

mesh_conv_t mesh_msgs_conv_of(const mesh_msg_t *m) {
    if (m->to != MESH_ADDR_BROADCAST)
        return MESH_CONV_DM(m->from == mesh_db.my_node_num ? m->to : m->from);
    return MESH_CONV_CHANNEL(m->channel);
}

// Rebuild the conversation table, most recently active first
static void reindex(void) {
    nconvs = 0;
    for (int i = count - 1; i >= 0; i--) {
        mesh_conv_t c = mesh_msgs_conv_of(&msgs[i]);
        int j = 0;
        while (j < nconvs && !same(convs[j].conv, c))
            j++;
        if (j == nconvs) {
            if (nconvs == MAX_CONVS)
                continue;
            memset(&convs[nconvs], 0, sizeof convs[0]);
            convs[nconvs].conv = c;
            convs[nconvs].last_seq = msgs[i].seq;
            nconvs++;
        }
        convs[j].count++;
        convs[j].unread += !read_flag[i];
    }
    version++;
}

void mesh_msgs_clear(void) {
    count = 0;
    reindex();
}

mesh_msg_t *mesh_msgs_add(mesh_conv_t conv, const mesh_msg_t *src) {
    if (count == MAX_MSGS)
        return NULL;
    for (int i = 0; src->id && i < count; i++)
        if (msgs[i].id == src->id && msgs[i].from == src->from)
            return NULL;
    mesh_msg_t *m = &msgs[count];
    *m = *src;
    m->seq = ++seq;
    m->channel = conv.channel;
    if (MESH_CONV_IS_DM(conv))
        m->to = m->from == mesh_db.my_node_num ? conv.peer : mesh_db.my_node_num;
    else
        m->to = MESH_ADDR_BROADCAST;
    m->text[MESH_MSG_TEXT_MAX] = '\0';
    m->len = (uint8_t)strlen(m->text);
    read_flag[count++] = m->status != MESH_MSG_RECEIVED;
    reindex();
    return m;
}

uint32_t mesh_msgs_version(void) { return version; }
int mesh_msgs_conv_count(void) { return nconvs; }

const mesh_conv_info_t *mesh_msgs_conv_at(int i) {
    return i >= 0 && i < nconvs ? &convs[i] : NULL;
}

const mesh_conv_info_t *mesh_msgs_conv(mesh_conv_t c) {
    for (int i = 0; i < nconvs; i++)
        if (same(convs[i].conv, c))
            return &convs[i];
    return NULL;
}

int mesh_msgs_count(mesh_conv_t c) {
    const mesh_conv_info_t *ci = mesh_msgs_conv(c);
    return ci ? ci->count : 0;
}

const mesh_msg_t *mesh_msgs_recent(mesh_conv_t c, int idx) {
    for (int i = count - 1; i >= 0; i--)
        if (same(mesh_msgs_conv_of(&msgs[i]), c) && idx-- == 0)
            return &msgs[i];
    return NULL;
}

void mesh_msgs_mark_read(mesh_conv_t c) {
    bool changed = false;
    for (int i = 0; i < count; i++)
        if (!read_flag[i] && same(mesh_msgs_conv_of(&msgs[i]), c))
            read_flag[i] = changed = true;
    if (changed)
        reindex();
}

uint32_t mesh_msgs_unread_total(void) {
    uint32_t n = 0;
    for (int i = 0; i < nconvs; i++)
        n += convs[i].unread;
    return n;
}

const char *mesh_msgs_status_name(const mesh_msg_t *m) {
    static const char *const names[] = {"received", "queued", "sent",
                                        "relayed", "delivered", "failed"};
    return m->status < 6 ? names[m->status] : "?";
}

mesh_send_result_t mesh_link_send_text(mesh_conv_t conv, const char *text,
                                       size_t len, uint32_t reply_id,
                                       mesh_msg_t **out) {
    if (out)
        *out = NULL;
    if (fake_send_result != MESH_SEND_OK)
        return fake_send_result;
    if (!len)
        return MESH_SEND_EMPTY;
    if (len > MESH_MSG_SEND_MAX)
        return MESH_SEND_TOO_LONG;
    mesh_msg_t m = {0};
    m.id = 0x7000 + seq;
    m.from = mesh_db.my_node_num;
    m.reply_id = reply_id;
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

uint32_t mesh_link_epoch(void) { return fake_epoch; }
