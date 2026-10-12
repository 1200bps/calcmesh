#include "trace.h"

#include <stdio.h>
#include <string.h>

#include "link.h"
#include "msgs.h"
#include "nodedb.h"

#include "meshtastic/mesh.pb.h"

static mesh_trace_t traces[MESH_TRACES];
static int count;
static int latest = -1; // index of the one sent last
static uint32_t last_sent_ms;
static bool sent_any;

void mesh_trace_clear(void) {
    memset(traces, 0, sizeof traces);
    count = 0;
    latest = -1;
    sent_any = false;
}

uint32_t mesh_trace_wait_ms(void) {
    if (!sent_any)
        return 0;
    uint32_t since = mesh_link_ms() - last_sent_ms;
    return since >= MESH_TRACE_GAP_MS ? 0 : MESH_TRACE_GAP_MS - since;
}

static mesh_trace_t *by_node(uint32_t node) {
    for (int i = 0; i < count; i++)
        if (traces[i].node == node)
            return &traces[i];
    return NULL;
}

static mesh_trace_t *by_id(uint32_t id) {
    for (int i = 0; i < count; i++)
        if (id && traces[i].id == id)
            return &traces[i];
    return NULL;
}

static void expire(mesh_trace_t *t) {
    if (t && t->state == TRACE_WAITING && mesh_link_ms() - t->sent_ms >= t->wait_ms) {
        t->state = TRACE_FAILED;
        t->error = TRACE_ERR_NO_ANSWER;
        printf("[trace] no answer from !%08lx\n", (unsigned long)t->node);
    }
}

mesh_trace_send_t mesh_trace_send(uint32_t node) {
    if (!node || node == MESH_ADDR_BROADCAST || node == mesh_db.my_node_num)
        return TRACE_BAD_NODE;
    if (mesh_link_state() != MESH_LINK_READY)
        return TRACE_NO_LINK;
    if (mesh_trace_wait_ms())
        return TRACE_TOO_SOON;
    uint32_t id;
    uint8_t hops;
    if (!mesh_link_send_traceroute(node, &id, &hops))
        return TRACE_NO_LINK;
    last_sent_ms = mesh_link_ms();
    sent_any = true;

    // The node's slot, else a free one, else the one sent longest ago
    mesh_trace_t *t = by_node(node);
    if (!t && count < MESH_TRACES)
        t = &traces[count++];
    if (!t) {
        t = &traces[0];
        for (int i = 1; i < count; i++)
            if (last_sent_ms - traces[i].sent_ms > last_sent_ms - t->sent_ms)
                t = &traces[i];
    }
    memset(t, 0, sizeof *t);
    t->node = node;
    t->id = id;
    t->sent_ms = last_sent_ms;
    t->wait_ms = (hops + 1u) * MESH_TRACE_HOP_MS;
    t->epoch = mesh_link_epoch();
    t->state = TRACE_WAITING;
    latest = (int)(t - traces);
    printf("[trace] tracing !%08lx, waiting up to %lus\n", (unsigned long)node,
           (unsigned long)(t->wait_ms / 1000));
    return TRACE_SENT;
}

const mesh_trace_t *mesh_trace_find(uint32_t node) {
    mesh_trace_t *t = by_node(node);
    expire(t);
    return t;
}

const mesh_trace_t *mesh_trace_latest(void) {
    if (latest < 0)
        return NULL;
    expire(&traces[latest]);
    return &traces[latest];
}

// Copy one direction. An SNR list that doesn't have one more entry than the
// route can't be lined up with it, so it counts as unknown.
static void take(uint32_t *dst, int8_t *snr, uint8_t *n, const uint32_t *route,
                 pb_size_t nroute, const int8_t *src_snr, pb_size_t nsnr) {
    if (nroute > MESH_TRACE_HOPS)
        nroute = MESH_TRACE_HOPS;
    *n = (uint8_t)nroute;
    memcpy(dst, route, nroute * sizeof *route);
    bool fits = nsnr == nroute + 1;
    for (int i = 0; i <= nroute; i++)
        snr[i] = fits ? src_snr[i] : MESH_TRACE_SNR_UNKNOWN;
}

void mesh_trace_answer(uint32_t request_id, uint32_t from, const meshtastic_RouteDiscovery *rd) {
    mesh_trace_t *t = by_id(request_id);
    if (!t || from != t->node)
        return;
    // Even after we gave up on it: late is better than nothing
    t->state = TRACE_DONE;
    t->error = 0;
    t->took_ms = mesh_link_ms() - t->sent_ms;
    take(t->out, t->snr_out, &t->n_out, rd->route, rd->route_count, rd->snr_towards,
         rd->snr_towards_count);
    // Older firmware leaves the way back out altogether; ours always adds
    // the SNR it heard the answer at, so an empty snr_back means that
    t->back_known = rd->snr_back_count > 0;
    take(t->back, t->snr_back, &t->n_back, rd->route_back, rd->route_back_count, rd->snr_back,
         rd->snr_back_count);
    mesh_trace_print(t);
}

void mesh_trace_routing(uint32_t request_id, uint32_t from, uint8_t error) {
    (void)from;
    mesh_trace_t *t = by_id(request_id);
    // An ack says nothing; the answer is what we wait for
    if (!t || t->state != TRACE_WAITING || !error)
        return;
    t->state = TRACE_FAILED;
    t->error = error;
    printf("[trace] !%08lx: %s\n", (unsigned long)t->node, mesh_routing_error_name(error));
}

void mesh_trace_notice(uint32_t reply_id, const char *text) {
    mesh_trace_t *t = by_id(reply_id);
    if (!t || t->state != TRACE_WAITING)
        return;
    t->state = TRACE_FAILED;
    t->error = TRACE_ERR_REFUSED;
    snprintf(t->note, sizeof t->note, "%s", text);
}

static void print_hop(uint32_t node, int8_t snr) {
    if (node == MESH_TRACE_UNKNOWN)
        printf(" > ?");
    else {
        const mesh_node_t *n = mesh_db_find(node);
        if (n && n->has_user && n->short_name[0])
            printf(" > %s", n->short_name);
        else
            printf(" > !%08lx", (unsigned long)node);
    }
    if (snr == MESH_TRACE_SNR_UNKNOWN)
        printf(" (?)");
    else
        printf(" (%.2f dB)", snr / 4.0);
}

static void print_start(uint32_t node) {
    const mesh_node_t *n = mesh_db_find(node);
    if (n && n->has_user && n->short_name[0])
        printf("%s", n->short_name);
    else
        printf("!%08lx", (unsigned long)node);
}

void mesh_trace_print(const mesh_trace_t *t) {
    if (!t)
        return;
    if (t->state == TRACE_WAITING) {
        printf("[trace] !%08lx: waiting (%lus)\n", (unsigned long)t->node,
               (unsigned long)((mesh_link_ms() - t->sent_ms) / 1000));
        return;
    }
    if (t->state == TRACE_FAILED) {
        printf("[trace] !%08lx: %s\n", (unsigned long)t->node,
               t->error == TRACE_ERR_NO_ANSWER ? "no answer"
               : t->error == TRACE_ERR_REFUSED ? t->note
                                               : mesh_routing_error_name(t->error));
        return;
    }
    printf("[trace] !%08lx answered in %.1fs\n", (unsigned long)t->node, t->took_ms / 1000.0);
    printf("  out:  ");
    print_start(mesh_db.my_node_num);
    for (int i = 0; i < t->n_out; i++)
        print_hop(t->out[i], t->snr_out[i]);
    print_hop(t->node, t->snr_out[t->n_out]);
    printf("\n  back: ");
    if (!t->back_known) {
        printf("not reported\n");
        return;
    }
    print_start(t->node);
    for (int i = 0; i < t->n_back; i++)
        print_hop(t->back[i], t->snr_back[i]);
    print_hop(mesh_db.my_node_num, t->snr_back[t->n_back]);
    printf("\n");
}
