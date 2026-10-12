#include "link.h"

#include <stdio.h>
#include <string.h>

#include "frame.h"
#include "metrics.h"
#include "msgs.h"
#include "nodedb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include "meshtastic/admin.pb.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/telemetry.pb.h"

// The Python client sends this many START2 bytes and waits before its first
// frame, so a node whose serial port is idle or asleep resyncs.
#define WAKE_BYTES 32
#define WAKE_SETTLE_MS 100
// Re-ask if the dump hasn't finished this long after the last byte arrived.
#define CONFIG_STALL_MS 5000
// The node drops a serial client it hasn't heard from in a while; the
// Python client heartbeats every 300 s.
#define HEARTBEAT_MS 300000

// The node only sends its node list during a handshake; ask for it again
// (nodes only, MESH_NODES_REFRESH_MS) to catch whatever we missed since.
// Give up on a refresh that hasn't finished in REFRESH_STALL_MS.
#define REFRESH_STALL_MS 30000

// What the firmware uses when the LoRa config leaves hop_limit unset.
#define DEFAULT_HOP_LIMIT 3

// Nonces the firmware treats specially (config-only / nodes-only dumps).
#define NONCE_ONLY_CONFIG 69420
#define NONCE_ONLY_NODES 69421

static mesh_link_io_t io;
static frame_rx_t rx;
static mesh_link_state_t state;
static mesh_link_stats_t stats;
static uint32_t nonce;
static uint32_t state_since_ms;
static uint32_t last_rx_ms;
static uint32_t last_tx_ms;
// The last handshake attempt timed out without a byte from the node.
static bool node_silent;
static bool db_cleared;
static uint32_t packet_id;
// Admin packets we sent lately, to match routing errors to (a ring)
#define ADMIN_IDS 8
static uint32_t admin_ids[ADMIN_IDS];
static unsigned admin_next;
// Packets to single nodes we sent lately, so a routing error saying our node
// has no key for the destination can tell which node that was (a ring)
#define UNICAST_IDS 8
static struct {
    uint32_t id, to;
} unicast[UNICAST_IDS];
static unsigned unicast_next;
// Nodes we asked for their NodeInfo lately, and when (our clock)
#define NODEINFO_ASKED 8
static struct {
    uint32_t node, ms;
} nodeinfo_asked[NODEINFO_ASKED];
static unsigned nodeinfo_next;
static bool refreshing; // a nodes-only dump is coming in
static uint32_t refresh_ms; // when the node list last came, or was asked for
// Node clock anchor: epoch seconds seen at our clock's anchor_ms.
static uint32_t anchor_epoch;
static uint32_t anchor_ms;
static void (*on_ready)(void) = mesh_db_dump;

// Decode target lives in .bss: FromRadio is ~770 bytes and the core 0 stack
// is 2 KiB by default.
static meshtastic_FromRadio from_radio;

// Console text from the node, collected into lines.
//
// When we start listening partway through a frame (the port just opened, or
// the node was mid-packet), the rest of that frame arrives here as stray
// bytes. Protobuf rarely gets far without a byte no text console sends, but
// the bits before that byte, cut up by 0x0A and 0x0D bytes, can look like
// short lines of text. So:
// - a finished line is held, and printed once the next line also finishes
//   cleanly or the node goes quiet for a moment;
// - a junk byte drops the held line and everything else up to the next
//   frame or quiet spell;
// - a frame drops whatever stray bytes came just before it.
#define CONSOLE_QUIET_MS 100
static char console[128];
static size_t console_len;
static char console_held[sizeof console];
static size_t console_held_len;
static bool console_junk;
static uint8_t console_esc;  // 1: after ESC, 2: inside an ESC [ sequence
static uint8_t console_utf8; // UTF-8 continuation bytes still due

static uint32_t now(void) { return io.now_ms(io.ctx); }
static uint32_t msgs_clock(void) { return now(); }

static void set_state(mesh_link_state_t s) {
    state = s;
    state_since_ms = now();
}

// Outgoing message and its encoding, static for the same reason.
static meshtastic_ToRadio to_radio;
static uint8_t tx_buf[FRAME_HEADER_LEN + meshtastic_ToRadio_size];

static bool send_to_radio(void) {
    uint8_t *buf = tx_buf;
    pb_ostream_t os =
        pb_ostream_from_buffer(buf + FRAME_HEADER_LEN, sizeof tx_buf - FRAME_HEADER_LEN);
    if (!pb_encode(&os, meshtastic_ToRadio_fields, &to_radio)) {
        printf("[link] ToRadio encode failed: %s\n", PB_GET_ERROR(&os));
        return false;
    }
    frame_header(buf, os.bytes_written);
    io.write(buf, FRAME_HEADER_LEN + os.bytes_written, io.ctx);
    last_tx_ms = now();
    if (to_radio.which_payload_variant == meshtastic_ToRadio_packet_tag &&
        to_radio.packet.to != MESH_ADDR_BROADCAST && to_radio.packet.to != mesh_db.my_node_num) {
        unicast[unicast_next % UNICAST_IDS].id = to_radio.packet.id;
        unicast[unicast_next % UNICAST_IDS].to = to_radio.packet.to;
        unicast_next++;
    }
    return true;
}

static void send_want_config(void) {
    do {
        nonce = nonce * 1103515245u + 12345u + now();
    } while (nonce == 0 || nonce == NONCE_ONLY_CONFIG || nonce == NONCE_ONLY_NODES);

    memset(&to_radio, 0, sizeof to_radio);
    to_radio.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
    to_radio.want_config_id = nonce;
    send_to_radio();
    stats.requests++;
    db_cleared = false;
    refreshing = false;
    printf("[link] want_config sent, nonce %lu\n", (unsigned long)nonce);
    set_state(MESH_LINK_CONFIGURING);
}

// The node list again, while we stay READY: the node pauses passing on
// packets while it sends it (they wait in its queue), but takes ours as usual.
static void send_nodes_refresh(void) {
    memset(&to_radio, 0, sizeof to_radio);
    to_radio.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
    to_radio.want_config_id = NONCE_ONLY_NODES;
    if (!send_to_radio())
        return;
    mesh_db_dump_begin(true);
    refreshing = true;
    refresh_ms = now();
}

static void send_heartbeat(void) {
    memset(&to_radio, 0, sizeof to_radio);
    to_radio.which_payload_variant = meshtastic_ToRadio_heartbeat_tag;
    send_to_radio();
}

void mesh_link_init(const mesh_link_io_t *io_) {
    io = *io_;
    frame_rx_init(&rx);
    memset(&stats, 0, sizeof stats);
    console_len = console_held_len = 0;
    console_junk = false;
    console_esc = console_utf8 = 0;
    node_silent = false;
    nonce = io.seed ^ now();
    packet_id = io.seed * 2654435761u ^ now();
    anchor_epoch = 0;
    memset(admin_ids, 0, sizeof admin_ids);
    memset(unicast, 0, sizeof unicast);
    memset(nodeinfo_asked, 0, sizeof nodeinfo_asked);
    refreshing = false;
    mesh_msgs_set_clock(msgs_clock);
    last_rx_ms = last_tx_ms = now();
    mesh_db_clear();
    set_state(MESH_LINK_IDLE);
}

void mesh_link_request_config(void) {
    uint8_t wake[WAKE_BYTES];
    memset(wake, FRAME_START2, sizeof wake);
    io.write(wake, sizeof wake, io.ctx);
    last_tx_ms = now();
    set_state(MESH_LINK_WAKING);
}

void mesh_link_set_passive(bool passive) {
    if (passive && state != MESH_LINK_PASSIVE) {
        refreshing = false;
        set_state(MESH_LINK_PASSIVE);
    } else if (!passive && state == MESH_LINK_PASSIVE) {
        mesh_link_request_config();
    }
}

mesh_link_state_t mesh_link_state(void) { return state; }
const mesh_link_stats_t *mesh_link_stats(void) { return &stats; }
void mesh_link_on_ready(void (*cb)(void)) { on_ready = cb; }

static void console_print_held(void) {
    if (!console_held_len)
        return;
    console_held[console_held_len] = '\0';
    printf("[node] %s\n", console_held);
    stats.console_lines++;
    console_held_len = 0;
}

static void console_reset_line(void) {
    console_len = 0;
    console_esc = console_utf8 = 0;
}

// Everything not yet printed is junk.
static void console_drop(void) {
    if (console_held_len || console_len || console_junk)
        stats.console_dropped++;
    console_held_len = 0;
    console_reset_line();
}

static void console_end_line(void) {
    if (!console_len)
        return;
    console_print_held(); // this line came after it cleanly
    memcpy(console_held, console, console_len);
    console_held_len = console_len;
    console_reset_line();
}

static void console_junk_byte(void) {
    console_drop();
    console_junk = true;
}

static void console_byte(uint8_t b) {
    // NULs turn up around node resets; skip them like the old filter did.
    if (console_junk || b == 0)
        return;
    if (b == '\n' || b == '\r') {
        console_end_line();
        return;
    }

    // Colour codes (ESC [ ... letter) are dropped.
    if (console_esc == 1) {
        console_esc = b == '[' ? 2 : 0;
        return;
    }
    if (console_esc == 2) {
        if (b >= 0x40 && b <= 0x7e)
            console_esc = 0;
        else if (b < 0x20 || b > 0x3f)
            console_junk_byte();
        return;
    }
    if (b == 0x1b) {
        console_esc = 1;
        return;
    }

    if (b == '\t')
        b = ' ';
    if (console_utf8) {
        if ((b & 0xc0) != 0x80) {
            console_junk_byte();
            return;
        }
        console_utf8--;
    } else {
        if ((b < 0x20 || b >= 0x7f) && (b < 0xc2 || b > 0xf4)) {
            // Control bytes, DEL, stray continuation bytes, bad UTF-8 leads
            console_junk_byte();
            return;
        }
        // A long line goes out in pieces, split between characters.
        if (console_len > sizeof console - 5)
            console_end_line();
        if (b >= 0xc2)
            console_utf8 = b >= 0xf0 ? 3 : b >= 0xe0 ? 2 : 1;
    }
    console[console_len++] = (char)b;
}

static const char *log_level(meshtastic_LogRecord_Level l) {
    switch (l) {
    case meshtastic_LogRecord_Level_CRITICAL:
        return "CRIT";
    case meshtastic_LogRecord_Level_ERROR:
        return "ERROR";
    case meshtastic_LogRecord_Level_WARNING:
        return "WARN";
    case meshtastic_LogRecord_Level_INFO:
        return "INFO";
    case meshtastic_LogRecord_Level_DEBUG:
        return "DEBUG";
    case meshtastic_LogRecord_Level_TRACE:
        return "TRACE";
    default:
        return "LOG";
    }
}

// The first record of our dump means the node accepted the request; only
// then throw away the old tables, so a request the node never answers
// leaves the previous picture intact. The node table stays: what the dump
// doesn't list is kept as cached (nodedb.h).
static void begin_dump(void) {
    if (state == MESH_LINK_CONFIGURING && !db_cleared) {
        mesh_db_dump_begin(false);
        db_cleared = true;
    }
}

static uint32_t next_packet_id(void);

static uint32_t unicast_dest(uint32_t id) {
    for (int i = 0; i < UNICAST_IDS; i++)
        if (id && unicast[i].id == id)
            return unicast[i].to;
    return 0;
}

static int asked_slot(uint32_t node) {
    for (int i = 0; i < NODEINFO_ASKED; i++)
        if (node && nodeinfo_asked[i].node == node)
            return i;
    return -1;
}

uint32_t mesh_link_nodeinfo_asked(uint32_t node) {
    int i = asked_slot(node);
    return i < 0 ? 0 : nodeinfo_asked[i].ms ? nodeinfo_asked[i].ms : 1;
}

bool mesh_link_send_nodeinfo_req(uint32_t to) {
    if (state != MESH_LINK_READY || !mesh_db.has_owner || !to || to == MESH_ADDR_BROADCAST ||
        to == mesh_db.my_node_num)
        return false;
    memset(&to_radio, 0, sizeof to_radio);
    to_radio.which_payload_variant = meshtastic_ToRadio_packet_tag;
    meshtastic_MeshPacket *p = &to_radio.packet;
    p->to = to;
    // NodeInfo never goes PKI; it has to be on a channel the node is on
    const mesh_node_t *n = mesh_db_find(to);
    p->channel = n ? n->channel : 0;
    p->id = next_packet_id();
    p->hop_limit = mesh_db.hop_limit ? mesh_db.hop_limit : DEFAULT_HOP_LIMIT;
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = meshtastic_PortNum_NODEINFO_APP;
    p->decoded.want_response = true;
    // Ours goes with it, key included, so it can send to us too
    pb_ostream_t os = pb_ostream_from_buffer(p->decoded.payload.bytes,
                                             sizeof p->decoded.payload.bytes);
    if (!pb_encode(&os, meshtastic_User_fields, &mesh_db.owner))
        return false;
    p->decoded.payload.size = (pb_size_t)os.bytes_written;
    if (!send_to_radio())
        return false;
    int slot = asked_slot(to);
    if (slot < 0)
        slot = (int)(nodeinfo_next++ % NODEINFO_ASKED);
    nodeinfo_asked[slot].node = to;
    nodeinfo_asked[slot].ms = now();
    stats.nodeinfo_tx++;
    printf("[link] asked !%08lx for its NodeInfo\n", (unsigned long)to);
    return true;
}

// A send failed for want of a key: ours for the node (our node refused, 39)
// or the node's for us (it couldn't decrypt, 35). Swapping NodeInfo fixes
// either, if the node answers; once per node per NODEINFO_RETRY_MS.
static void key_missing(uint32_t node) {
    int i = asked_slot(node);
    if (i >= 0 && now() - nodeinfo_asked[i].ms < MESH_NODEINFO_RETRY_MS)
        return;
    mesh_link_send_nodeinfo_req(node);
}

static void set_status(mesh_msg_t *m, mesh_msg_status_t st, uint8_t err) {
    uint8_t before = m->status;
    mesh_msgs_set_status(m, st, err);
    if (m->status != before)
        mesh_msgs_print(m);
}

// The node lost its transmit queue: nothing still waiting will be answered.
static void fail_in_flight(void) {
    for (int c = 0; c < mesh_msgs_conv_count(); c++) {
        mesh_conv_t conv = mesh_msgs_conv_at(c)->conv;
        for (int i = 0; i < mesh_msgs_count(conv); i++) {
            mesh_msg_t *m = (mesh_msg_t *)mesh_msgs_recent(conv, i);
            if (m->status == MESH_MSG_QUEUED || m->status == MESH_MSG_SENT)
                set_status(m, MESH_MSG_FAILED, meshtastic_Routing_Error_TIMEOUT);
        }
    }
}

static uint8_t hops_of(const meshtastic_MeshPacket *p) {
    if (!p->hop_start || p->hop_limit > p->hop_start)
        return 0xFF;
    return (uint8_t)(p->hop_start - p->hop_limit);
}

static void handle_text(const meshtastic_MeshPacket *p) {
    const meshtastic_Data *d = &p->decoded;
    static mesh_msg_t m; // ~270 bytes; keep it off the stack
    memset(&m, 0, sizeof m);
    m.id = p->id;
    m.from = p->from;
    m.to = p->to;
    m.channel = p->channel;
    m.rx_time = p->has_rx_time ? p->rx_time : 0;
    m.reply_id = d->reply_id;
    m.emoji = d->emoji != 0;
    m.via_mqtt = p->via_mqtt;
    m.snr = p->rx_snr;
    m.rssi = p->has_rx_rssi ? (int16_t)p->rx_rssi : 0;
    m.hops = hops_of(p);
    size_t n = d->payload.size;
    if (n > MESH_MSG_TEXT_MAX)
        n = MESH_MSG_TEXT_MAX;
    memcpy(m.text, d->payload.bytes, n);
    m.text[n] = '\0'; // also stops at an embedded NUL

    mesh_conv_t conv;
    uint32_t me = mesh_db.my_node_num;
    if (p->from == me && me) {
        // Sent by another client of our node (its own screen, say), echoed
        // to us: keep it as ours. Acks for it will still find it by id.
        m.status = MESH_MSG_SENT;
        m.hops = 0xFF;
        conv = p->to == MESH_ADDR_BROADCAST ? MESH_CONV_CHANNEL(p->channel)
                                            : MESH_CONV_DM(p->to);
    } else if (p->to == MESH_ADDR_BROADCAST) {
        m.status = MESH_MSG_RECEIVED;
        conv = MESH_CONV_CHANNEL(p->channel);
    } else if (p->to == me || !me) {
        m.status = MESH_MSG_RECEIVED;
        conv = MESH_CONV_DM(p->from);
    } else {
        printf("[link] text for !%08lx, not us; ignored\n", (unsigned long)p->to);
        return;
    }
    const mesh_msg_t *stored = mesh_msgs_add(conv, &m);
    if (stored) {
        stats.texts_rx++;
        mesh_msgs_print(stored);
    }
}

static mesh_admin_cb admin_cb;
static mesh_admin_routing_cb admin_routing_cb;

void mesh_link_on_admin(mesh_admin_cb on_admin, mesh_admin_routing_cb on_routing) {
    admin_cb = on_admin;
    admin_routing_cb = on_routing;
}

static bool is_admin_id(uint32_t id) {
    for (int i = 0; i < ADMIN_IDS; i++)
        if (id && admin_ids[i] == id)
            return true;
    return false;
}

static void handle_routing(const meshtastic_MeshPacket *p) {
    uint32_t req = p->decoded.request_id;
    mesh_msg_t *m = mesh_msgs_find_outgoing(req);
    bool admin = !m && is_admin_id(req);
    meshtastic_Routing r = meshtastic_Routing_init_zero;
    pb_istream_t is = pb_istream_from_buffer(p->decoded.payload.bytes, p->decoded.payload.size);
    if (!req || !pb_decode(&is, meshtastic_Routing_fields, &r) ||
        r.which_variant != meshtastic_Routing_error_reason_tag)
        return;
    if (r.error_reason == meshtastic_Routing_Error_PKI_SEND_FAIL_PUBLIC_KEY &&
        p->from == mesh_db.my_node_num)
        key_missing(unicast_dest(req));
    else if (r.error_reason == meshtastic_Routing_Error_PKI_UNKNOWN_PUBKEY &&
             p->from != mesh_db.my_node_num)
        key_missing(p->from);
    if (!m && !admin) {
        // Perhaps a telemetry request's
        mesh_metrics_routing(req, p->from, (uint8_t)r.error_reason);
        return;
    }
    if (admin) {
        if (admin_routing_cb)
            admin_routing_cb(req, p->from, (uint8_t)r.error_reason);
        if (r.error_reason != meshtastic_Routing_Error_NONE) {
            stats.admin_errors++;
            stats.admin_last_error = (uint8_t)r.error_reason;
            printf("[link] node refused admin packet %08lx: routing error %d\n",
                   (unsigned long)req, r.error_reason);
        }
        return;
    }
    if (r.error_reason != meshtastic_Routing_Error_NONE)
        set_status(m, MESH_MSG_FAILED, (uint8_t)r.error_reason);
    else if (m->to != MESH_ADDR_BROADCAST && p->from == m->to)
        set_status(m, MESH_MSG_DELIVERED, 0);
    else
        // An implicit ack: our node heard someone pass it on.
        set_status(m, MESH_MSG_RELAYED, 0);
}

static void handle_position(const meshtastic_MeshPacket *p) {
    meshtastic_Position pos = meshtastic_Position_init_zero;
    pb_istream_t is = pb_istream_from_buffer(p->decoded.payload.bytes, p->decoded.payload.size);
    if (!pb_decode(&is, meshtastic_Position_fields, &pos))
        return;
    if (mesh_db_set_position(p->from, &pos))
        printf("[link] position !%08lx %.5f,%.5f\n", (unsigned long)p->from,
               pos.latitude_i / 1e7, pos.longitude_i / 1e7);
}

static void handle_nodeinfo(const meshtastic_MeshPacket *p) {
    if (p->from == mesh_db.my_node_num)
        return; // ours comes in the dump
    static meshtastic_User u; // ~180 bytes
    memset(&u, 0, sizeof u);
    pb_istream_t is = pb_istream_from_buffer(p->decoded.payload.bytes, p->decoded.payload.size);
    if (!pb_decode(&is, meshtastic_User_fields, &u))
        return;
    const mesh_node_t *n = mesh_db_set_user(p->from, &u);
    if (n)
        printf("[link] nodeinfo !%08lx %s (%s)%s\n", (unsigned long)p->from, n->long_name,
               n->short_name, n->has_key ? "" : ", no key");
}

static void handle_telemetry(const meshtastic_MeshPacket *p) {
    static meshtastic_Telemetry t; // ~300 bytes
    memset(&t, 0, sizeof t);
    pb_istream_t is = pb_istream_from_buffer(p->decoded.payload.bytes, p->decoded.payload.size);
    if (!pb_decode(&is, meshtastic_Telemetry_fields, &t))
        return;
    uint32_t time = p->has_rx_time ? p->rx_time : 0;
    if (mesh_metrics_add(p->from, &t, time))
        printf("[link] telemetry !%08lx: %s%s\n", (unsigned long)p->from,
               t.which_variant == meshtastic_Telemetry_device_metrics_tag        ? "device"
               : t.which_variant == meshtastic_Telemetry_environment_metrics_tag ? "environment"
               : t.which_variant == meshtastic_Telemetry_power_metrics_tag       ? "power"
                                                                                 : "local stats",
               p->decoded.request_id ? " (an answer)" : "");
    // Even a variant we don't keep answers the request
    mesh_metrics_answer(p->decoded.request_id, p->from);
}

static void handle_packet(const meshtastic_MeshPacket *p) {
    if (p->has_rx_time && p->rx_time) {
        anchor_epoch = p->rx_time;
        anchor_ms = now();
    }
    if (p->which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
        printf("[link] packet !%08lx -> !%08lx ch %u (encrypted)\n",
               (unsigned long)p->from, (unsigned long)p->to, p->channel);
        return;
    }
    if (p->from != mesh_db.my_node_num)
        // A PKI packet comes in on channel 0 whichever channel the node uses
        mesh_db_heard(p->from, p->has_rx_time ? p->rx_time : 0, p->rx_snr, hops_of(p),
                      p->pki_encrypted ? MESH_CHANNEL_UNKNOWN : (uint8_t)p->channel,
                      p->via_mqtt);
    switch (p->decoded.portnum) {
    case meshtastic_PortNum_NODEINFO_APP:
        handle_nodeinfo(p);
        break;
    case meshtastic_PortNum_POSITION_APP:
        handle_position(p);
        break;
    case meshtastic_PortNum_TEXT_MESSAGE_APP:
        handle_text(p);
        break;
    case meshtastic_PortNum_ROUTING_APP:
        handle_routing(p);
        break;
    case meshtastic_PortNum_TELEMETRY_APP:
        handle_telemetry(p);
        break;
    case meshtastic_PortNum_ADMIN_APP: {
        static meshtastic_AdminMessage a; // ~500 bytes
        memset(&a, 0, sizeof a);
        pb_istream_t is = pb_istream_from_buffer(p->decoded.payload.bytes, p->decoded.payload.size);
        if (!pb_decode(&is, meshtastic_AdminMessage_fields, &a)) {
            printf("[link] admin packet from !%08lx didn't decode\n", (unsigned long)p->from);
            break;
        }
        if (admin_cb)
            admin_cb(p, &a);
        break;
    }
    default:
        printf("[link] packet !%08lx -> !%08lx ch %u port %d, %u bytes\n",
               (unsigned long)p->from, (unsigned long)p->to, p->channel,
               p->decoded.portnum, (unsigned)p->decoded.payload.size);
        break;
    }
}

static void handle_from_radio(const meshtastic_FromRadio *fr) {
    switch (fr->which_payload_variant) {
    case meshtastic_FromRadio_my_info_tag:
        begin_dump();
        mesh_db_set_my_info(&fr->my_info);
        printf("[link] my_info: node !%08lx, expecting %u nodes\n",
               (unsigned long)fr->my_info.my_node_num, fr->my_info.nodedb_count);
        break;
    case meshtastic_FromRadio_metadata_tag:
        begin_dump();
        mesh_db_set_metadata(&fr->metadata);
        break;
    case meshtastic_FromRadio_node_info_tag:
        begin_dump();
        if (!mesh_db_upsert_node(&fr->node_info))
            printf("[link] node table full, dropped !%08lx\n",
                   (unsigned long)fr->node_info.num);
        if (fr->node_info.has_device_metrics)
            mesh_metrics_nodeinfo(fr->node_info.num, &fr->node_info.device_metrics,
                                  fr->node_info.last_heard);
        break;
    case meshtastic_FromRadio_channel_tag:
        begin_dump();
        mesh_db_set_channel(&fr->channel);
        break;
    case meshtastic_FromRadio_config_tag:
        begin_dump();
        mesh_db_set_config(&fr->config);
        break;
    case meshtastic_FromRadio_moduleConfig_tag:
        begin_dump();
        mesh_db_set_module_config(&fr->moduleConfig);
        break;
    case meshtastic_FromRadio_config_complete_id_tag:
        if (refreshing && fr->config_complete_id == NONCE_ONLY_NODES) {
            refreshing = false;
            mesh_db_dump_end();
            printf("[link] node list refreshed: %u nodes\n", mesh_db.node_count);
            break;
        }
        if (state != MESH_LINK_CONFIGURING || fr->config_complete_id != nonce) {
            printf("[link] ignoring stale config_complete %lu\n",
                   (unsigned long)fr->config_complete_id);
            break;
        }
        printf("[link] config complete: %u nodes, %lu frames\n",
               mesh_db.node_count, (unsigned long)stats.frames);
        mesh_db_dump_end();
        set_state(MESH_LINK_READY);
        refresh_ms = now();
        stats.syncs++;
        if (on_ready)
            on_ready();
        break;
    case meshtastic_FromRadio_rebooted_tag:
        if (state == MESH_LINK_PASSIVE)
            break;
        printf("[link] node rebooted, re-requesting config\n");
        fail_in_flight();
        mesh_link_request_config();
        break;
    case meshtastic_FromRadio_log_record_tag:
        printf("[node %s] %s%s%s\n", log_level(fr->log_record.level),
               fr->log_record.source, fr->log_record.source[0] ? ": " : "",
               fr->log_record.message);
        break;
    case meshtastic_FromRadio_packet_tag:
        handle_packet(&fr->packet);
        break;
    case meshtastic_FromRadio_queueStatus_tag: {
        const meshtastic_QueueStatus *q = &fr->queueStatus;
        mesh_msg_t *m = mesh_msgs_find_outgoing(q->mesh_packet_id);
        if (m && q->res == 0)
            set_status(m, MESH_MSG_SENT, 0);
        else if (q->res != 0)
            printf("[link] node queue error %d for packet %08lx\n", q->res,
                   (unsigned long)q->mesh_packet_id);
        break;
    }
    case meshtastic_FromRadio_clientNotification_tag:
        printf("[node notice] %s\n", fr->clientNotification.message);
        break;
    default:
        // fileInfo, deviceuiConfig, region_presets
        // and friends: not needed yet.
        break;
    }
}

static void handle_frame(const uint8_t *buf, size_t len) {
    pb_istream_t is = pb_istream_from_buffer(buf, len);
    memset(&from_radio, 0, sizeof from_radio);
    if (!pb_decode(&is, meshtastic_FromRadio_fields, &from_radio)) {
        stats.bad_frames++;
        printf("[link] FromRadio decode failed (%u bytes): %s\n", (unsigned)len,
               PB_GET_ERROR(&is));
        return;
    }
    stats.frames++;
    handle_from_radio(&from_radio);
}

void mesh_link_rx(const uint8_t *data, size_t len) {
    if (len) {
        if (now() - last_rx_ms >= CONSOLE_QUIET_MS) {
            console_print_held();
            console_junk = false;
        }
        last_rx_ms = now();
        node_silent = false;
    }
    for (size_t i = 0; i < len; i++) {
        switch (frame_rx_push(&rx, data[i])) {
        case FRAME_RX_READY:
            console_drop();
            console_junk = false;
            handle_frame(rx.buf, rx.len);
            break;
        case FRAME_RX_STRAY:
            console_byte(data[i]);
            break;
        case FRAME_RX_NONE:
            break;
        }
    }
}

void mesh_link_poll(void) {
    uint32_t t = now();
    if (console_held_len && t - last_rx_ms >= CONSOLE_QUIET_MS)
        console_print_held();
    switch (state) {
    case MESH_LINK_WAKING:
        if (t - state_since_ms >= WAKE_SETTLE_MS)
            send_want_config();
        break;
    case MESH_LINK_CONFIGURING: {
        // Measure the stall from whichever is later: our request or the
        // last byte the node sent.
        uint32_t since = state_since_ms;
        if ((int32_t)(last_rx_ms - since) > 0)
            since = last_rx_ms;
        if (t - since >= CONFIG_STALL_MS) {
            node_silent = since == state_since_ms;
            printf("[link] %s, retrying\n",
                   node_silent ? "no answer from the node" : "no config_complete");
            mesh_link_request_config();
        }
        break;
    }
    case MESH_LINK_READY:
        if (refreshing && t - refresh_ms >= REFRESH_STALL_MS) {
            // Unfinished: nothing becomes cached on a partial list
            printf("[link] node list refresh didn't finish\n");
            refreshing = false;
        }
        if (!refreshing && t - refresh_ms >= MESH_NODES_REFRESH_MS)
            send_nodes_refresh();
        else if (t - last_tx_ms >= HEARTBEAT_MS)
            send_heartbeat();
        break;
    default:
        break;
    }
}

mesh_link_conn_t mesh_link_conn(void) {
    switch (state) {
    case MESH_LINK_READY:
        return MESH_CONN_UP;
    case MESH_LINK_PASSIVE:
        return MESH_CONN_DESKTOP;
    case MESH_LINK_WAKING:
    case MESH_LINK_CONFIGURING:
        return node_silent ? MESH_CONN_NONE : MESH_CONN_SYNCING;
    default:
        return MESH_CONN_NONE;
    }
}

uint32_t mesh_link_epoch(void) {
    if (!anchor_epoch)
        return 0;
    return anchor_epoch + (now() - anchor_ms) / 1000u;
}

static uint32_t next_packet_id(void) {
    do {
        packet_id = packet_id * 1664525u + 1013904223u;
    } while (packet_id == 0);
    return packet_id;
}

mesh_send_result_t mesh_link_send_text(mesh_conv_t conv, const char *text,
                                       size_t len, uint32_t reply_id,
                                       mesh_msg_t **out) {
    if (out)
        *out = NULL;
    if (state != MESH_LINK_READY)
        return MESH_SEND_NOT_READY;
    if (!len)
        return MESH_SEND_EMPTY;
    if (len > MESH_MSG_SEND_MAX)
        return MESH_SEND_TOO_LONG;
    if (memchr(text, '\0', len))
        len = strlen(text);
    if (!conv.peer) {
        uint8_t role = conv.channel < MESH_MAX_CHANNELS
                           ? mesh_db.channels[conv.channel].role
                           : meshtastic_Channel_Role_DISABLED;
        if (role == meshtastic_Channel_Role_DISABLED)
            return MESH_SEND_NO_CHANNEL;
    }

    memset(&to_radio, 0, sizeof to_radio);
    to_radio.which_payload_variant = meshtastic_ToRadio_packet_tag;
    meshtastic_MeshPacket *p = &to_radio.packet;
    p->to = conv.peer ? conv.peer : MESH_ADDR_BROADCAST;
    // DMs go out on the primary channel (or PKI, which the node picks).
    p->channel = conv.peer ? 0 : conv.channel;
    p->id = next_packet_id();
    p->want_ack = true;
    // The node takes this as given (and copies it to hop_start), so a 0
    // would never be rebroadcast. Clients send the configured limit.
    p->hop_limit = mesh_db.hop_limit ? mesh_db.hop_limit : DEFAULT_HOP_LIMIT;
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p->decoded.reply_id = reply_id;
    memcpy(p->decoded.payload.bytes, text, len);
    p->decoded.payload.size = (pb_size_t)len;

    // Store first, so nothing goes on air that we can't track.
    static mesh_msg_t m;
    memset(&m, 0, sizeof m);
    m.id = p->id;
    m.from = mesh_db.my_node_num;
    m.to = p->to;
    m.channel = p->channel;
    m.rx_time = mesh_link_epoch();
    m.reply_id = reply_id;
    m.hops = 0xFF;
    m.status = MESH_MSG_QUEUED;
    memcpy(m.text, text, len);
    mesh_msg_t *stored = mesh_msgs_add(conv, &m);
    if (!stored)
        return MESH_SEND_FAILED;
    if (!send_to_radio()) {
        mesh_msgs_set_status(stored, MESH_MSG_FAILED, meshtastic_Routing_Error_TOO_LARGE);
        return MESH_SEND_FAILED;
    }
    stats.texts_tx++;
    if (out)
        *out = stored;
    return MESH_SEND_OK;
}

uint32_t mesh_link_ms(void) { return now(); }

bool mesh_link_send_admin(const meshtastic_AdminMessage *a) {
    return mesh_link_send_admin_to(mesh_db.my_node_num, a, false, NULL);
}

bool mesh_link_send_admin_to(uint32_t to, const meshtastic_AdminMessage *a, bool want_response,
                             uint32_t *id) {
    if (state != MESH_LINK_READY || !mesh_db.my_node_num || !to)
        return false;
    memset(&to_radio, 0, sizeof to_radio);
    to_radio.which_payload_variant = meshtastic_ToRadio_packet_tag;
    meshtastic_MeshPacket *p = &to_radio.packet;
    // from stays 0: that's what tells the node it came from its own client
    p->to = to;
    p->channel = 0;
    p->id = next_packet_id();
    if (to == mesh_db.my_node_num) {
        p->hop_limit = 0;
    } else {
        // Over the air: only with PKI (the node refuses to send it otherwise),
        // acked, and as far as our other packets go
        p->pki_encrypted = true;
        p->want_ack = true;
        p->hop_limit = mesh_db.hop_limit ? mesh_db.hop_limit : DEFAULT_HOP_LIMIT;
        p->decoded.want_response = want_response;
    }
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = meshtastic_PortNum_ADMIN_APP;
    pb_ostream_t os = pb_ostream_from_buffer(p->decoded.payload.bytes,
                                             sizeof p->decoded.payload.bytes);
    if (!pb_encode(&os, meshtastic_AdminMessage_fields, a)) {
        printf("[link] AdminMessage encode failed: %s\n", PB_GET_ERROR(&os));
        return false;
    }
    p->decoded.payload.size = (pb_size_t)os.bytes_written;
    if (!send_to_radio())
        return false;
    admin_ids[admin_next++ % ADMIN_IDS] = p->id;
    stats.admin_tx++;
    if (id)
        *id = p->id;
    return true;
}

bool mesh_link_send_telemetry_req(uint32_t to, pb_size_t variant, uint32_t *id) {
    if (state != MESH_LINK_READY || !mesh_db.my_node_num || !to)
        return false;
    // The request is a Telemetry with the wanted variant present but empty
    static meshtastic_Telemetry t;
    memset(&t, 0, sizeof t);
    t.which_variant = variant;
    memset(&to_radio, 0, sizeof to_radio);
    to_radio.which_payload_variant = meshtastic_ToRadio_packet_tag;
    meshtastic_MeshPacket *p = &to_radio.packet;
    p->to = to;
    p->channel = 0; // or PKI, which the node picks when it has the key
    p->id = next_packet_id();
    p->hop_limit = to == mesh_db.my_node_num ? 0
                   : mesh_db.hop_limit       ? mesh_db.hop_limit
                                             : DEFAULT_HOP_LIMIT;
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = meshtastic_PortNum_TELEMETRY_APP;
    p->decoded.want_response = true;
    pb_ostream_t os = pb_ostream_from_buffer(p->decoded.payload.bytes,
                                             sizeof p->decoded.payload.bytes);
    if (!pb_encode(&os, meshtastic_Telemetry_fields, &t))
        return false;
    p->decoded.payload.size = (pb_size_t)os.bytes_written;
    if (!send_to_radio())
        return false;
    if (id)
        *id = p->id;
    return true;
}

const char *mesh_link_send_result_name(mesh_send_result_t r) {
    switch (r) {
    case MESH_SEND_OK:
        return "ok";
    case MESH_SEND_NOT_READY:
        return "not connected to the node";
    case MESH_SEND_EMPTY:
        return "empty message";
    case MESH_SEND_TOO_LONG:
        return "too long";
    case MESH_SEND_NO_CHANNEL:
        return "no such channel";
    case MESH_SEND_FAILED:
        return "failed";
    default:
        return "?";
    }
}
