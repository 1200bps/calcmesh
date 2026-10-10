#include "msgs.h"

#include <stdio.h>
#include <string.h>

#include "meshtastic/mesh.pb.h"
#include "nodedb.h"

#define CONV_SLOTS (MESH_MAX_CHANNELS + MESH_MAX_DMS)

typedef struct {
    mesh_conv_info_t info;
    bool used;
} conv_slot_t;

// Message slots, and their order: order[0..held) are slot indices, oldest
// first. Evicting shuffles only order[], so a message never moves while held.
static mesh_msg_t pool[MESH_MSG_POOL];
static uint8_t pool_conv[MESH_MSG_POOL]; // conv slot of each held message
static uint8_t order[MESH_MSG_POOL];
static int held;

// Conversation slots: channels at their own index, DMs after them. conv_order
// lists the used ones, most recently active first.
static conv_slot_t convs[CONV_SLOTS];
static uint8_t conv_order[CONV_SLOTS];
static int conv_used;

static uint32_t next_seq = 1;
static uint32_t version;
static uint32_t (*clock_ms)(void);
static const mesh_msgs_observer_t *observer;

_Static_assert(MESH_MSG_POOL <= 255, "order[] holds uint8_t slot indices");
_Static_assert(CONV_SLOTS <= 255, "pool_conv[] holds uint8_t conv indices");

void mesh_msgs_clear(void) {
    memset(pool, 0, sizeof pool);
    memset(convs, 0, sizeof convs);
    held = conv_used = 0;
    version++;
}

void mesh_msgs_set_clock(uint32_t (*now_ms)(void)) { clock_ms = now_ms; }
void mesh_msgs_set_observer(const mesh_msgs_observer_t *obs) { observer = obs; }
uint32_t mesh_msgs_version(void) { return version; }

static int conv_find(mesh_conv_t c) {
    if (!c.peer)
        return c.channel < MESH_MAX_CHANNELS && convs[c.channel].used ? c.channel : -1;
    for (int i = MESH_MAX_CHANNELS; i < CONV_SLOTS; i++)
        if (convs[i].used && convs[i].info.conv.peer == c.peer)
            return i;
    return -1;
}

static void conv_order_remove(int slot) {
    for (int i = 0; i < conv_used; i++) {
        if (conv_order[i] == slot) {
            memmove(&conv_order[i], &conv_order[i + 1], (size_t)(conv_used - i - 1));
            conv_used--;
            return;
        }
    }
}

// Move a conversation to the front of conv_order.
static void conv_touch(int slot) {
    conv_order_remove(slot);
    memmove(&conv_order[1], &conv_order[0], (size_t)conv_used);
    conv_order[0] = (uint8_t)slot;
    conv_used++;
}

static void remove_at(int pos) {
    int s = order[pos];
    conv_slot_t *c = &convs[pool_conv[s]];
    c->info.count--;
    memmove(&order[pos], &order[pos + 1], (size_t)(held - pos - 1));
    held--;
    if (!c->info.count) {
        conv_order_remove(pool_conv[s]);
        c->used = false;
    }
}

static void drop_conv(int slot) {
    for (int i = held - 1; i >= 0; i--)
        if (pool_conv[order[i]] == slot)
            remove_at(i);
}

void mesh_msgs_drop(mesh_conv_t c) {
    int slot = conv_find(c);
    if (slot >= 0) {
        drop_conv(slot);
        version++;
    }
}

static int conv_get(mesh_conv_t c) {
    int slot = conv_find(c);
    if (slot >= 0)
        return slot;
    if (!c.peer) {
        if (c.channel >= MESH_MAX_CHANNELS)
            return -1;
        slot = c.channel;
    } else {
        for (int i = MESH_MAX_CHANNELS; i < CONV_SLOTS; i++) {
            if (!convs[i].used) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            // All DM slots taken: drop the one quiet for longest.
            for (int i = conv_used - 1; i >= 0; i--) {
                if (conv_order[i] >= MESH_MAX_CHANNELS) {
                    slot = conv_order[i];
                    break;
                }
            }
            if (slot < 0)
                return -1;
            drop_conv(slot);
        }
    }
    memset(&convs[slot], 0, sizeof convs[slot]);
    convs[slot].used = true;
    convs[slot].info.conv = c.peer ? MESH_CONV_DM(c.peer) : MESH_CONV_CHANNEL(c.channel);
    return slot;
}

// Free a message slot: from the conversation holding the most messages, its
// oldest.
static int evict(int keep_conv) {
    int busiest = -1;
    for (int i = 0; i < CONV_SLOTS; i++)
        if (convs[i].used &&
            (busiest < 0 || convs[i].info.count > convs[busiest].info.count))
            busiest = i;
    // The conversation we're adding to may be the busiest; that's fine, it
    // just loses its own oldest message. keep_conv only matters if it would
    // otherwise be emptied and freed under us.
    if (busiest == keep_conv && convs[busiest].info.count == 1)
        return -1;
    for (int i = 0; i < held; i++) {
        if (pool_conv[order[i]] == busiest) {
            int s = order[i];
            remove_at(i);
            return s;
        }
    }
    return -1;
}

static int free_slot(void) {
    // Slots not referenced by order[] are free.
    static uint8_t used[MESH_MSG_POOL];
    memset(used, 0, sizeof used);
    for (int i = 0; i < held; i++)
        used[order[i]] = 1;
    for (int i = 0; i < MESH_MSG_POOL; i++)
        if (!used[i])
            return i;
    return -1;
}

static mesh_msg_t *add(mesh_conv_t conv, const mesh_msg_t *m, bool restored) {
    int existing = conv_find(conv);
    if (existing >= 0 && m->id) {
        for (int i = 0; i < held; i++) {
            const mesh_msg_t *o = &pool[order[i]];
            if (pool_conv[order[i]] == existing && o->id == m->id && o->from == m->from)
                return NULL;
        }
    }

    int cs = conv_get(conv);
    if (cs < 0)
        return NULL;
    int s = held < MESH_MSG_POOL ? free_slot() : evict(cs);
    if (s < 0) {
        if (!convs[cs].info.count)
            convs[cs].used = false;
        return NULL;
    }

    mesh_msg_t *dst = &pool[s];
    *dst = *m;
    dst->text[MESH_MSG_TEXT_MAX] = '\0';
    dst->len = (uint8_t)strlen(dst->text);
    dst->seq = next_seq++;
    if (!restored || !dst->local_ms)
        dst->local_ms = clock_ms ? clock_ms() : 0;
    pool_conv[s] = (uint8_t)cs;
    order[held++] = (uint8_t)s;

    conv_slot_t *c = &convs[cs];
    c->info.count++;
    c->info.last_seq = dst->seq;
    if (!restored && dst->status == MESH_MSG_RECEIVED)
        c->info.unread++;
    conv_touch(cs);
    version++;

    if (!restored && observer && observer->added)
        observer->added(dst, observer->ctx);
    return dst;
}

mesh_msg_t *mesh_msgs_add(mesh_conv_t conv, const mesh_msg_t *m) {
    return add(conv, m, false);
}

mesh_msg_t *mesh_msgs_restore(mesh_conv_t conv, const mesh_msg_t *m) {
    return add(conv, m, true);
}

int mesh_msgs_conv_count(void) { return conv_used; }

const mesh_conv_info_t *mesh_msgs_conv_at(int i) {
    if (i < 0 || i >= conv_used)
        return NULL;
    return &convs[conv_order[i]].info;
}

const mesh_conv_info_t *mesh_msgs_conv(mesh_conv_t conv) {
    int s = conv_find(conv);
    return s < 0 ? NULL : &convs[s].info;
}

int mesh_msgs_count(mesh_conv_t conv) {
    const mesh_conv_info_t *c = mesh_msgs_conv(conv);
    return c ? c->count : 0;
}

const mesh_msg_t *mesh_msgs_recent(mesh_conv_t conv, int i) {
    int cs = conv_find(conv);
    if (cs < 0 || i < 0)
        return NULL;
    for (int k = held - 1; k >= 0; k--)
        if (pool_conv[order[k]] == cs && i-- == 0)
            return &pool[order[k]];
    return NULL;
}

void mesh_msgs_mark_read(mesh_conv_t conv) {
    int s = conv_find(conv);
    if (s >= 0 && convs[s].info.unread) {
        convs[s].info.unread = 0;
        version++;
    }
}

uint32_t mesh_msgs_unread_total(void) {
    uint32_t n = 0;
    for (int i = 0; i < CONV_SLOTS; i++)
        if (convs[i].used)
            n += convs[i].info.unread;
    return n;
}

mesh_conv_t mesh_msgs_conv_of(const mesh_msg_t *m) {
    uintptr_t p = (uintptr_t)m, base = (uintptr_t)pool;
    if (p >= base && p < base + sizeof pool)
        return convs[pool_conv[(p - base) / sizeof *m]].info.conv;
    // Not one of ours: work it out from the addresses.
    if (m->to != MESH_ADDR_BROADCAST)
        return MESH_CONV_DM(m->status == MESH_MSG_RECEIVED ? m->from : m->to);
    return MESH_CONV_CHANNEL(m->channel);
}

mesh_msg_t *mesh_msgs_find_outgoing(uint32_t id) {
    if (!id)
        return NULL;
    for (int i = held - 1; i >= 0; i--) {
        mesh_msg_t *m = &pool[order[i]];
        if (m->id == id && m->status != MESH_MSG_RECEIVED)
            return m;
    }
    return NULL;
}

void mesh_msgs_set_status(mesh_msg_t *m, mesh_msg_status_t status, uint8_t error) {
    // DELIVERED and FAILED are final; otherwise only forward, or to FAILED.
    if (m->status == MESH_MSG_RECEIVED || m->status == MESH_MSG_DELIVERED ||
        m->status == MESH_MSG_FAILED || status == MESH_MSG_RECEIVED)
        return;
    if (status <= m->status && status != MESH_MSG_FAILED)
        return;
    m->status = (uint8_t)status;
    m->error = status == MESH_MSG_FAILED ? error : 0;
    version++;
    if (observer && observer->updated)
        observer->updated(m, observer->ctx);
}

static const char *routing_error_name(uint8_t e) {
    switch (e) {
    case meshtastic_Routing_Error_NO_ROUTE:
        return "no route";
    case meshtastic_Routing_Error_GOT_NAK:
        return "refused";
    case meshtastic_Routing_Error_TIMEOUT:
        return "timed out";
    case meshtastic_Routing_Error_NO_INTERFACE:
        return "no radio";
    case meshtastic_Routing_Error_MAX_RETRANSMIT:
        return "no ack";
    case meshtastic_Routing_Error_NO_CHANNEL:
        return "no such channel";
    case meshtastic_Routing_Error_TOO_LARGE:
        return "too large";
    case meshtastic_Routing_Error_NO_RESPONSE:
        return "no response";
    case meshtastic_Routing_Error_DUTY_CYCLE_LIMIT:
        return "duty cycle limit";
    case meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED:
        return "rate limited";
    case meshtastic_Routing_Error_PKI_FAILED:
    case meshtastic_Routing_Error_PKI_UNKNOWN_PUBKEY:
    case meshtastic_Routing_Error_PKI_SEND_FAIL_PUBLIC_KEY:
        return "no key for node";
    default:
        return "error";
    }
}

const char *mesh_msgs_status_name(const mesh_msg_t *m) {
    static char buf[32];
    switch (m->status) {
    case MESH_MSG_RECEIVED:
        return "received";
    case MESH_MSG_QUEUED:
        return "sending";
    case MESH_MSG_SENT:
        return "sent";
    case MESH_MSG_RELAYED:
        return "relayed";
    case MESH_MSG_DELIVERED:
        return "delivered";
    case MESH_MSG_FAILED:
        snprintf(buf, sizeof buf, "failed: %s", routing_error_name(m->error));
        return buf;
    default:
        return "?";
    }
}

static void node_label(uint32_t num, char *buf, size_t len) {
    const mesh_node_t *n = mesh_db_find(num);
    if (n && n->has_user && n->short_name[0])
        snprintf(buf, len, "%s", n->short_name);
    else
        snprintf(buf, len, "!%08lx", (unsigned long)num);
}

void mesh_msgs_print(const mesh_msg_t *m) {
    char who[12], peer[12];
    mesh_conv_t c = mesh_msgs_conv_of(m);
    if (c.peer) {
        node_label(c.peer, peer, sizeof peer);
        printf("[dm %s] ", peer);
    } else {
        printf("[ch%u %s] ", c.channel, mesh_db_channel_name(c.channel));
    }
    if (m->status == MESH_MSG_RECEIVED) {
        node_label(m->from, who, sizeof who);
        printf("%s: %s  (", who, m->text);
        if (m->hops != 0xFF)
            printf("%u hop%s, ", m->hops, m->hops == 1 ? "" : "s");
        printf("snr %.1f%s)\n", (double)m->snr, m->via_mqtt ? ", mqtt" : "");
    } else {
        printf("me: %s  (%s, id %08lx)\n", m->text, mesh_msgs_status_name(m),
               (unsigned long)m->id);
    }
}
