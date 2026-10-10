// Drives the link against a scripted fake node: the node side decodes what the
// link writes and answers with framed FromRadio records.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame.h"
#include "cmd.h"
#include "link.h"
#include "metrics.h"
#include "msgs.h"
#include "nodedb.h"
#include "nodeedit.h"
#include "remoteadmin.h"
#include "meshtastic/admin.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

static int failures;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
            failures++;                                                        \
        }                                                                      \
    } while (0)

// ---- fake transport --------------------------------------------------------

static uint32_t clock_ms;
static uint8_t tx[4096];
static size_t tx_len;

static void fake_write(const uint8_t *d, size_t n, void *ctx) {
    (void)ctx;
    assert(tx_len + n <= sizeof tx);
    memcpy(tx + tx_len, d, n);
    tx_len += n;
}
static uint32_t fake_now(void *ctx) {
    (void)ctx;
    return clock_ms;
}

// Parse everything the link has written since last call; return the last
// want_config nonce seen (0 if none), count heartbeats, and keep the last
// MeshPacket sent.
static int heartbeats, wake_bytes, packets;
static meshtastic_MeshPacket last_packet;
// Admin messages the link sent, decoded
#define MAX_ADMIN 16
static meshtastic_AdminMessage admins[MAX_ADMIN];
static meshtastic_MeshPacket admin_packets[MAX_ADMIN];
static int n_admin;
static uint32_t node_read(void) {
    frame_rx_t rx;
    frame_rx_init(&rx);
    uint32_t want = 0;
    for (size_t i = 0; i < tx_len; i++) {
        frame_rx_result_t r = frame_rx_push(&rx, tx[i]);
        if (r == FRAME_RX_STRAY && tx[i] == FRAME_START2)
            wake_bytes++;
        if (r != FRAME_RX_READY)
            continue;
        meshtastic_ToRadio m = meshtastic_ToRadio_init_zero;
        pb_istream_t is = pb_istream_from_buffer(rx.buf, rx.len);
        CHECK(pb_decode(&is, meshtastic_ToRadio_fields, &m));
        if (m.which_payload_variant == meshtastic_ToRadio_want_config_id_tag)
            want = m.want_config_id;
        if (m.which_payload_variant == meshtastic_ToRadio_heartbeat_tag)
            heartbeats++;
        if (m.which_payload_variant == meshtastic_ToRadio_packet_tag) {
            packets++;
            last_packet = m.packet;
            if (m.packet.decoded.portnum == meshtastic_PortNum_ADMIN_APP && n_admin < MAX_ADMIN) {
                meshtastic_AdminMessage a = meshtastic_AdminMessage_init_zero;
                pb_istream_t as = pb_istream_from_buffer(m.packet.decoded.payload.bytes,
                                                         m.packet.decoded.payload.size);
                CHECK(pb_decode(&as, meshtastic_AdminMessage_fields, &a));
                admin_packets[n_admin] = m.packet;
                admins[n_admin++] = a;
            }
        }
    }
    tx_len = 0;
    return want;
}

// ---- fake node output --------------------------------------------------------

static uint8_t stream[65536];
static size_t stream_len;

static void emit_raw(const void *d, size_t n) {
    assert(stream_len + n <= sizeof stream);
    memcpy(stream + stream_len, d, n);
    stream_len += n;
}

static void emit(const meshtastic_FromRadio *fr) {
    uint8_t buf[FRAME_HEADER_LEN + meshtastic_FromRadio_size];
    pb_ostream_t os = pb_ostream_from_buffer(buf + 4, sizeof buf - 4);
    if (!pb_encode(&os, meshtastic_FromRadio_fields, fr)) {
        fprintf(stderr, "encode: %s\n", PB_GET_ERROR(&os));
        exit(1);
    }
    frame_header(buf, os.bytes_written);
    emit_raw(buf, 4 + os.bytes_written);
}

// Deliver the queued stream in awkward chunk sizes.
static void flush_stream(void) {
    size_t i = 0, chunk = 1;
    while (i < stream_len) {
        size_t n = chunk < stream_len - i ? chunk : stream_len - i;
        mesh_link_rx(stream + i, n);
        i += n;
        chunk = chunk * 7 % 61 + 1;
    }
    stream_len = 0;
}

static meshtastic_FromRadio fr_my_info(uint32_t num, uint16_t count) {
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    fr.my_info.my_node_num = num;
    fr.my_info.nodedb_count = count;
    strcpy(fr.my_info.pio_env, "heltec-v4");
    return fr;
}

static meshtastic_FromRadio fr_node(uint32_t num, const char *lname,
                                    const char *sname, uint32_t heard) {
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    meshtastic_NodeInfo *n = &fr.node_info;
    n->num = num;
    if (lname) {
        n->has_user = true;
        snprintf(n->user.id, sizeof n->user.id, "!%08x", num);
        strcpy(n->user.long_name, lname);
        strcpy(n->user.short_name, sname);
        n->user.hw_model = meshtastic_HardwareModel_HELTEC_V3;
    }
    n->last_heard = heard;
    n->snr = 6.25f;
    n->has_hops_away = true;
    n->hops_away = 2;
    n->has_device_metrics = true;
    n->device_metrics.has_battery_level = true;
    n->device_metrics.battery_level = 87;
    n->has_position = true;
    n->position.has_latitude_i = n->position.has_longitude_i = true;
    n->position.latitude_i = 401234567;
    n->position.longitude_i = -751234567;
    return fr;
}

static meshtastic_FromRadio fr_channel(int idx, meshtastic_Channel_Role role,
                                       const char *name, int psk_len) {
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_channel_tag;
    fr.channel.index = idx;
    fr.channel.role = role;
    fr.channel.has_settings = true;
    strcpy(fr.channel.settings.name, name);
    fr.channel.settings.psk.size = psk_len;
    memset(fr.channel.settings.psk.bytes, 1, psk_len);
    return fr;
}

static meshtastic_FromRadio fr_lora(meshtastic_Config_LoRaConfig_ModemPreset p) {
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_config_tag;
    fr.config.which_payload_variant = meshtastic_Config_lora_tag;
    fr.config.payload_variant.lora.use_preset = true;
    fr.config.payload_variant.lora.modem_preset = p;
    fr.config.payload_variant.lora.region = meshtastic_Config_LoRaConfig_RegionCode_US;
    fr.config.payload_variant.lora.hop_limit = 5;
    return fr;
}

static meshtastic_FromRadio fr_complete(uint32_t id) {
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_config_complete_id_tag;
    fr.config_complete_id = id;
    return fr;
}

static void advance(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i += 10) {
        clock_ms += 10;
        mesh_link_poll();
    }
}

static int ready_calls;
static void on_ready(void) {
    ready_calls++;
    mesh_db_dump();
}

// ---- tests ---------------------------------------------------------------------

static void test_frame_resync(void) {
    frame_rx_t rx;
    frame_rx_init(&rx);
    // Oversized length is dropped; a 0x94 0x94 0xC3 false start still syncs.
    const uint8_t in[] = {0x94, 0xC3, 0x7F, 0xFF, 'h', 'i', 0x94, 0x94, 0xC3,
                          0x00, 0x02, 0x08, 0x2A};
    int ready = 0, stray = 0;
    for (size_t i = 0; i < sizeof in; i++) {
        frame_rx_result_t r = frame_rx_push(&rx, in[i]);
        ready += r == FRAME_RX_READY;
        stray += r == FRAME_RX_STRAY;
    }
    CHECK(rx.dropped == 1);
    CHECK(stray == 2); // 'h', 'i'
    CHECK(ready == 1);
    CHECK(rx.len == 2 && rx.buf[0] == 0x08 && rx.buf[1] == 0x2A);
}

static void test_handshake(void) {
    mesh_link_io_t io = {.write = fake_write, .now_ms = fake_now};
    clock_ms = 1000;
    mesh_link_init(&io);
    mesh_link_on_ready(on_ready);
    mesh_link_request_config();

    CHECK(mesh_link_state() == MESH_LINK_WAKING);
    advance(50);
    CHECK(node_read() == 0); // still settling after the wake bytes
    CHECK(wake_bytes == 32);
    advance(100);
    uint32_t nonce = node_read();
    CHECK(nonce != 0);
    CHECK(mesh_link_state() == MESH_LINK_CONFIGURING);

    const uint32_t me = 0xa1b2c3d4;
    meshtastic_FromRadio fr;

    // Leftover console text and a stale completion from an earlier session.
    emit_raw("INFO  | ??:??:?? 3 [Main] Power on\r\n", 37);
    flush_stream();
    advance(200);
    node_read();
    fr = fr_complete(12345);
    emit(&fr);

    fr = fr_my_info(me, 4);
    emit(&fr);
    fr = fr_node(me, "Calcmesh Base", "CALC", 1700000000);
    emit(&fr);
    fr = fr_channel(0, meshtastic_Channel_Role_PRIMARY, "", 1);
    emit(&fr);
    fr = fr_channel(1, meshtastic_Channel_Role_SECONDARY, "Ops", 32);
    emit(&fr);
    fr = fr_channel(2, meshtastic_Channel_Role_DISABLED, "", 0);
    emit(&fr);
    fr = fr_lora(meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST);
    emit(&fr);
    fr = fr_node(0x11111111, "Hilltop Router", "HILL", 1700000000 - 600);
    emit(&fr);
    fr = fr_node(0x22222222, NULL, NULL, 0); // never sent a user record
    emit(&fr);
    fr = fr_node(0x33333333, "Mobile", "MOB", 1700000000 - 90000);
    emit(&fr);
    fr = fr_complete(nonce);
    emit(&fr);
    flush_stream();

    CHECK(mesh_link_state() == MESH_LINK_READY);
    CHECK(mesh_link_conn() == MESH_CONN_UP);
    CHECK(ready_calls == 1);
    CHECK(mesh_link_stats()->bad_frames == 0);
    CHECK(mesh_link_stats()->console_lines == 1);
    CHECK(mesh_db.my_node_num == me);
    CHECK(strcmp(mesh_db.pio_env, "heltec-v4") == 0);
    CHECK(strcmp(mesh_db_channel_name(0), "MediumFast") == 0);
    CHECK(strcmp(mesh_db_channel_name(1), "Ops") == 0);
    CHECK(mesh_db.channels[1].psk_len == 32);
    CHECK(mesh_db.channels[2].role == meshtastic_Channel_Role_DISABLED);
    CHECK(mesh_db.hop_limit == 5);

#if MESH_MAX_NODES >= 4
    CHECK(mesh_db.node_count == 4);
    const mesh_node_t *n = mesh_db_find(0x11111111);
    CHECK(n && strcmp(n->long_name, "Hilltop Router") == 0);
    CHECK(n && n->hops_away == 2 && n->battery == 87 && n->has_position);
    n = mesh_db_find(0x22222222);
    CHECK(n && !n->has_user && strcmp(n->id, "!22222222") == 0);
#endif

    // Heartbeat after five quiet minutes.
    heartbeats = 0;
    advance(300000);
    node_read();
    CHECK(heartbeats == 1);

    // A node reboot restarts the handshake, and the old tables survive until
    // the node actually starts answering.
    fr = (meshtastic_FromRadio)meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_rebooted_tag;
    fr.rebooted = true;
    emit(&fr);
    flush_stream();
    CHECK(mesh_link_state() == MESH_LINK_WAKING);
    CHECK(mesh_link_conn() == MESH_CONN_SYNCING);
    advance(200);
    uint32_t nonce2 = node_read();
    CHECK(nonce2 != 0 && nonce2 != nonce);
    CHECK(mesh_db.node_count > 0);

    // Node goes silent: link retries after the stall timeout.
    advance(5200);
    CHECK(node_read() != 0);
    CHECK(mesh_link_stats()->requests == 3);
    CHECK(mesh_link_conn() == MESH_CONN_NONE);
    // and it's back as soon as the node says anything
    emit_raw("\r\n", 2);
    flush_stream();
    CHECK(mesh_link_conn() == MESH_CONN_SYNCING);
}

// The port opens partway through a frame: its tail arrives as stray bytes,
// which must not come out as console lines, while real console text (with
// colour codes and UTF-8) still does.
static void test_console(void) {
    mesh_link_io_t io = {.write = fake_write, .now_ms = fake_now};
    mesh_link_init(&io);
    mesh_link_on_ready(NULL);
    CHECK(mesh_link_conn() == MESH_CONN_NONE);

    meshtastic_FromRadio frames[3] = {
        fr_node(0x11111111, "Hilltop Router", "HILL", 1700000000),
        fr_channel(1, meshtastic_Channel_Role_SECONDARY, "Ops", 32),
        fr_my_info(0xa1b2c3d4, 40),
    };
    for (int f = 0; f < 3; f++) {
        emit(&frames[f]);
        uint8_t whole[sizeof stream];
        size_t len = stream_len;
        memcpy(whole, stream, len);
        stream_len = 0;
        // Every cut past the header, so the start bytes are gone
        for (size_t cut = 2; cut < len; cut++) {
            emit_raw(whole + cut, len - cut);
            emit(&frames[(f + 1) % 3]);
            flush_stream();
        }
    }
    CHECK(mesh_link_stats()->console_lines == 0);
    CHECK(mesh_link_stats()->console_dropped > 0);

    const char text[] = "\x1b[34mINFO \x1b[0m | 12:00:01 [Main] Caf\xc3\xa9 \xe2\x9c\x93\r\n"
                        "\x1b[33mWARN \x1b[0m | 12:00:02 [Main] ok\r\n";
    emit_raw(text, sizeof text - 1);
    flush_stream();
    CHECK(mesh_link_stats()->console_lines == 1); // the last one waits...
    advance(100);
    CHECK(mesh_link_stats()->console_lines == 2); // ...for a quiet moment

    // A line longer than the buffer comes out in pieces, none of them junk.
    char long_line[400];
    for (size_t i = 0; i < sizeof long_line - 2; i += 2)
        long_line[i] = '\xc3', long_line[i + 1] = '\xa9';
    long_line[sizeof long_line - 2] = '\r';
    long_line[sizeof long_line - 1] = '\n';
    uint32_t dropped = mesh_link_stats()->console_dropped;
    emit_raw(long_line, sizeof long_line);
    flush_stream();
    advance(100);
    CHECK(mesh_link_stats()->console_lines > 4);
    CHECK(mesh_link_stats()->console_dropped == dropped);
}

static void test_eviction(void) {
#if MESH_MAX_NODES == 4
    mesh_link_io_t io = {.write = fake_write, .now_ms = fake_now};
    mesh_link_init(&io);
    mesh_link_on_ready(NULL);
    mesh_link_request_config();
    advance(200);
    uint32_t nonce = node_read();
    meshtastic_FromRadio fr = fr_my_info(1, 6);
    emit(&fr);
    for (uint32_t i = 1; i <= 6; i++) {
        // Node 1 (us) is the oldest but must never be evicted.
        fr = fr_node(i, "n", "n", i == 1 ? 1 : 1000 + i);
        emit(&fr);
    }
    fr = fr_complete(nonce);
    emit(&fr);
    flush_stream();
    CHECK(mesh_link_state() == MESH_LINK_READY);
    CHECK(mesh_db.node_count == 4);
    CHECK(mesh_db.evicted == 2);
    CHECK(mesh_db_find(1) != NULL);
    CHECK(mesh_db_find(6) != NULL && mesh_db_find(5) != NULL);
    CHECK(mesh_db_find(2) == NULL);
#endif
}

// Line noise must never crash or wedge the parser.
static void test_noise(void) {
    mesh_link_io_t io = {.write = fake_write, .now_ms = fake_now};
    mesh_link_init(&io);
    mesh_link_on_ready(NULL);
    srand(1);
    uint8_t junk[4096];
    for (int round = 0; round < 64; round++) {
        for (size_t i = 0; i < sizeof junk; i++)
            junk[i] = (uint8_t)rand();
        // Plant plausible headers so the decoder sees plenty of garbage frames.
        for (size_t i = 0; i + 4 < sizeof junk; i += 37 + rand() % 200) {
            junk[i] = FRAME_START1;
            junk[i + 1] = FRAME_START2;
            junk[i + 2] = 0;
            junk[i + 3] = (uint8_t)(rand() % 64);
        }
        mesh_link_rx(junk, sizeof junk);
    }
    tx_len = 0;
}

// Desktop mode: a PC drives the node while the link only listens.
static void test_passive(void) {
    mesh_link_io_t io = {.write = fake_write, .now_ms = fake_now};
    mesh_link_init(&io);
    mesh_link_on_ready(NULL);
    mesh_link_request_config();
    advance(200);
    uint32_t nonce = node_read();
    CHECK(nonce != 0);

    // The PC takes over mid-handshake.
    mesh_link_set_passive(true);
    CHECK(mesh_link_state() == MESH_LINK_PASSIVE);
    CHECK(mesh_link_conn() == MESH_CONN_DESKTOP);

    // The PC's own dump goes by: tables follow it, its config_complete
    // isn't ours, and a reboot notice doesn't make us talk.
    meshtastic_FromRadio fr = fr_my_info(0xabcdef01, 2);
    emit(&fr);
    fr = fr_node(0x33333333, "Desk Node", "DESK", 1700000000);
    emit(&fr);
    fr = fr_complete(nonce + 1);
    emit(&fr);
    fr = (meshtastic_FromRadio)meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_rebooted_tag;
    fr.rebooted = true;
    emit(&fr);
    flush_stream();
    CHECK(mesh_db_find(0x33333333) != NULL);
    CHECK(mesh_link_state() == MESH_LINK_PASSIVE);

    // Silent for longer than every retry and heartbeat timer.
    wake_bytes = heartbeats = 0;
    advance(600000);
    CHECK(tx_len == 0);
    CHECK(node_read() == 0 && heartbeats == 0 && wake_bytes == 0);

    // PC lets go: wake, then a fresh handshake.
    mesh_link_set_passive(false);
    CHECK(mesh_link_state() == MESH_LINK_WAKING);
    advance(200);
    uint32_t nonce2 = node_read();
    CHECK(nonce2 != 0 && wake_bytes == 32);
    CHECK(mesh_link_state() == MESH_LINK_CONFIGURING);
}


// ---- messaging -------------------------------------------------------------------

static const uint32_t ME = 0xa1b2c3d4;

// Bring the link up with channels 0 (primary), 1 ("Ops") and 2 (disabled),
// and nodes HILL (0x11111111) and MOB (0x33333333).
static void link_up(void) {
    mesh_link_io_t io = {.write = fake_write, .now_ms = fake_now, .seed = 7};
    mesh_link_init(&io);
    mesh_msgs_clear();
    mesh_link_on_ready(NULL);
    mesh_link_request_config();
    advance(200);
    uint32_t nonce = node_read();
    meshtastic_FromRadio fr = fr_my_info(ME, 3);
    emit(&fr);
    fr = fr_node(ME, "Calcmesh Base", "CALC", 1700000000);
    emit(&fr);
    fr = fr_node(0x11111111, "Hilltop Router", "HILL", 1700000000);
    emit(&fr);
    fr = fr_node(0x33333333, "Mobile", "MOB", 1700000000);
    emit(&fr);
    fr = fr_channel(0, meshtastic_Channel_Role_PRIMARY, "", 1);
    emit(&fr);
    fr = fr_channel(1, meshtastic_Channel_Role_SECONDARY, "Ops", 32);
    emit(&fr);
    fr = fr_channel(2, meshtastic_Channel_Role_DISABLED, "", 0);
    emit(&fr);
    fr = fr_complete(nonce);
    emit(&fr);
    flush_stream();
    CHECK(mesh_link_state() == MESH_LINK_READY);
}

static meshtastic_FromRadio fr_text(uint32_t from, uint32_t to, uint8_t ch,
                                    uint32_t id, const char *text) {
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_packet_tag;
    meshtastic_MeshPacket *p = &fr.packet;
    p->from = from;
    p->to = to;
    p->channel = ch;
    p->id = id;
    p->has_rx_time = true;
    p->rx_time = 1700000100;
    p->rx_snr = -7.5f;
    p->has_rx_rssi = true;
    p->rx_rssi = -110;
    p->hop_start = 3;
    p->hop_limit = 1;
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p->decoded.payload.size = (pb_size_t)strlen(text);
    memcpy(p->decoded.payload.bytes, text, strlen(text));
    return fr;
}

static meshtastic_FromRadio fr_routing(uint32_t from, uint32_t request_id,
                                       meshtastic_Routing_Error err) {
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_packet_tag;
    meshtastic_MeshPacket *p = &fr.packet;
    p->from = from;
    p->to = ME;
    p->id = request_id ^ 0x5555;
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = meshtastic_PortNum_ROUTING_APP;
    p->decoded.request_id = request_id;
    meshtastic_Routing r = meshtastic_Routing_init_zero;
    r.which_variant = meshtastic_Routing_error_reason_tag;
    r.error_reason = err;
    pb_ostream_t os = pb_ostream_from_buffer(p->decoded.payload.bytes,
                                             sizeof p->decoded.payload.bytes);
    CHECK(pb_encode(&os, meshtastic_Routing_fields, &r));
    p->decoded.payload.size = (pb_size_t)os.bytes_written;
    return fr;
}

static meshtastic_FromRadio fr_queue(uint32_t id, int8_t res) {
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_queueStatus_tag;
    fr.queueStatus.mesh_packet_id = id;
    fr.queueStatus.res = res;
    fr.queueStatus.free = 15;
    fr.queueStatus.maxlen = 16;
    return fr;
}

static void deliver(meshtastic_FromRadio fr) {
    emit(&fr);
    flush_stream();
}

static mesh_msg_t *send_text(mesh_conv_t conv, const char *text) {
    mesh_msg_t *m = NULL;
    CHECK(mesh_link_send_text(conv, text, strlen(text), 0, &m) == MESH_SEND_OK);
    return m;
}

static void test_receive(void) {
    link_up();
    uint32_t v = mesh_msgs_version();

    deliver(fr_text(0x11111111, MESH_ADDR_BROADCAST, 0, 100, "hello mesh"));
    deliver(fr_text(0x33333333, ME, 0, 200, "psst"));
    deliver(fr_text(0x11111111, MESH_ADDR_BROADCAST, 1, 101, "ops check"));
    // The same packet again (say, after a reconnect) isn't stored twice.
    deliver(fr_text(0x11111111, MESH_ADDR_BROADCAST, 0, 100, "hello mesh"));
    // Addressed to someone else: ignored.
    deliver(fr_text(0x11111111, 0x33333333, 0, 102, "not for us"));
    // An embedded NUL ends the text.
    deliver(fr_text(0x33333333, ME, 0, 201, "a\0b"));

    CHECK(mesh_msgs_version() != v);
    CHECK(mesh_link_stats()->texts_rx == 4);
    CHECK(mesh_msgs_conv_count() == 3);
    CHECK(mesh_msgs_unread_total() == 4);

    // Most recently active first.
    const mesh_conv_info_t *c = mesh_msgs_conv_at(0);
    CHECK(c && c->conv.peer == 0x33333333 && c->count == 2 && c->unread == 2);
    c = mesh_msgs_conv_at(1);
    CHECK(c && c->conv.peer == 0 && c->conv.channel == 1);

    const mesh_msg_t *m = mesh_msgs_recent(MESH_CONV_CHANNEL(0), 0);
    CHECK(m && strcmp(m->text, "hello mesh") == 0 && m->len == 10);
    CHECK(m && m->from == 0x11111111 && m->status == MESH_MSG_RECEIVED);
    CHECK(m && m->hops == 2 && m->rssi == -110 && m->rx_time == 1700000100);
    CHECK(mesh_msgs_count(MESH_CONV_CHANNEL(0)) == 1);
    CHECK(mesh_msgs_recent(MESH_CONV_CHANNEL(0), 1) == NULL);

    m = mesh_msgs_recent(MESH_CONV_DM(0x33333333), 0);
    CHECK(m && strcmp(m->text, "a") == 0);
    m = mesh_msgs_recent(MESH_CONV_DM(0x33333333), 1);
    CHECK(m && strcmp(m->text, "psst") == 0);
    CHECK(m && mesh_msgs_conv_of(m).peer == 0x33333333);

    v = mesh_msgs_version();
    mesh_msgs_mark_read(MESH_CONV_DM(0x33333333));
    CHECK(mesh_msgs_version() != v);
    CHECK(mesh_msgs_unread_total() == 2);

    // Our node's clock, carried on received packets.
    clock_ms += 30000;
    CHECK(mesh_link_epoch() == 1700000130);
}

static meshtastic_FromRadio fr_position(uint32_t from, int32_t lat, int32_t lon,
                                        uint32_t rx_time) {
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_packet_tag;
    meshtastic_MeshPacket *p = &fr.packet;
    p->from = from;
    p->to = MESH_ADDR_BROADCAST;
    p->id = (uint32_t)lat ^ rx_time;
    p->has_rx_time = true;
    p->rx_time = rx_time;
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = meshtastic_PortNum_POSITION_APP;
    meshtastic_Position pos = meshtastic_Position_init_zero;
    pos.has_latitude_i = pos.has_longitude_i = true;
    pos.latitude_i = lat;
    pos.longitude_i = lon;
    pos.has_altitude = true;
    pos.altitude = 1234;
    pos.time = rx_time - 5;
    pos.precision_bits = 32;
    pb_ostream_t os = pb_ostream_from_buffer(p->decoded.payload.bytes,
                                             sizeof p->decoded.payload.bytes);
    CHECK(pb_encode(&os, meshtastic_Position_fields, &pos));
    p->decoded.payload.size = (pb_size_t)os.bytes_written;
    return fr;
}

static void test_position(void) {
    link_up();
    const mesh_node_t *hill = mesh_db_find(0x11111111);
    CHECK(hill && hill->has_position && hill->latitude_i == 401234567);

    deliver(fr_position(0x11111111, 473901234, -1224567890, 1700000500));
    CHECK(hill->has_position && hill->latitude_i == 473901234 &&
          hill->longitude_i == -1224567890 && hill->altitude == 1234);
    CHECK(hill->position_time == 1700000495 && hill->precision_bits == 32);
    CHECK(hill->last_heard == 1700000500);

    // No fix (0,0) keeps the last good one, but still counts as heard.
    deliver(fr_position(0x11111111, 0, 0, 1700000600));
    CHECK(hill->latitude_i == 473901234 && hill->last_heard == 1700000600);

    // An older packet doesn't move last_heard back.
    deliver(fr_text(0x11111111, MESH_ADDR_BROADCAST, 0, 999, "late"));
    CHECK(hill->last_heard == 1700000600);

    // From a node we've never seen: it's added, with its position.
    int before = mesh_db.node_count;
    deliver(fr_position(0x77777777, 473000000, -1220000000, 1700000700));
    const mesh_node_t *n = mesh_db_find(0x77777777);
    CHECK(mesh_db.node_count == before + 1);
    CHECK(n && n->has_position && !n->has_user && strcmp(n->id, "!77777777") == 0);
}

static void test_send(void) {
    link_up();
    node_read();
    packets = 0;

    // Channel message: queued, then the node takes it, then someone relays it.
    mesh_msg_t *m = send_text(MESH_CONV_CHANNEL(1), "on my way");
    CHECK(packets == 0);
    node_read();
    CHECK(packets == 1);
    CHECK(last_packet.to == MESH_ADDR_BROADCAST && last_packet.channel == 1);
    CHECK(last_packet.want_ack && last_packet.id != 0);
    CHECK(last_packet.hop_limit == 3); // no LoRa config sent: the default
    CHECK(last_packet.which_payload_variant == meshtastic_MeshPacket_decoded_tag);
    CHECK(last_packet.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP);
    CHECK(last_packet.decoded.payload.size == 9 &&
          memcmp(last_packet.decoded.payload.bytes, "on my way", 9) == 0);
    CHECK(m && m->id == last_packet.id && m->from == ME);
    CHECK(m && m->status == MESH_MSG_QUEUED);
    CHECK(mesh_msgs_unread_total() == 0); // our own messages aren't unread
    uint32_t id1 = last_packet.id;

    deliver(fr_queue(id1, 0));
    CHECK(m->status == MESH_MSG_SENT);
    deliver(fr_routing(ME, id1, meshtastic_Routing_Error_NONE));
    CHECK(m->status == MESH_MSG_RELAYED);
    CHECK(strcmp(mesh_msgs_status_name(m), "relayed") == 0);

    // DM: relayed, then the destination acks; a late relay can't undo that.
    m = send_text(MESH_CONV_DM(0x33333333), "you there?");
    node_read();
    CHECK(last_packet.to == 0x33333333 && last_packet.channel == 0);
    uint32_t id2 = last_packet.id;
    CHECK(id2 != id1);
    deliver(fr_routing(ME, id2, meshtastic_Routing_Error_NONE));
    CHECK(m->status == MESH_MSG_RELAYED);
    deliver(fr_routing(0x33333333, id2, meshtastic_Routing_Error_NONE));
    CHECK(m->status == MESH_MSG_DELIVERED);
    deliver(fr_routing(ME, id2, meshtastic_Routing_Error_NONE));
    CHECK(m->status == MESH_MSG_DELIVERED);

    // DM that never gets through.
    m = send_text(MESH_CONV_DM(0x11111111), "hello?");
    node_read();
    deliver(fr_routing(ME, last_packet.id, meshtastic_Routing_Error_MAX_RETRANSMIT));
    CHECK(m->status == MESH_MSG_FAILED);
    CHECK(strcmp(mesh_msgs_status_name(m), "failed: no ack") == 0);
    // An ack for an id we never sent is ignored.
    deliver(fr_routing(ME, 0xdeadbeef, meshtastic_Routing_Error_NONE));

    // Refusals, none of which put anything on the wire.
    mesh_msg_t *none;
    char big[MESH_MSG_SEND_MAX + 2];
    memset(big, 'x', sizeof big);
    CHECK(mesh_link_send_text(MESH_CONV_CHANNEL(2), "x", 1, 0, &none) == MESH_SEND_NO_CHANNEL);
    CHECK(none == NULL);
    CHECK(mesh_link_send_text(MESH_CONV_CHANNEL(9), "x", 1, 0, NULL) == MESH_SEND_NO_CHANNEL);
    CHECK(mesh_link_send_text(MESH_CONV_CHANNEL(0), "", 0, 0, NULL) == MESH_SEND_EMPTY);
    CHECK(mesh_link_send_text(MESH_CONV_CHANNEL(0), big, MESH_MSG_SEND_MAX + 1, 0, NULL) ==
          MESH_SEND_TOO_LONG);
    CHECK(mesh_link_send_text(MESH_CONV_CHANNEL(0), big, MESH_MSG_SEND_MAX, 0, NULL) ==
          MESH_SEND_OK);
    mesh_link_set_passive(true);
    CHECK(mesh_link_send_text(MESH_CONV_CHANNEL(0), "x", 1, 0, NULL) == MESH_SEND_NOT_READY);
    mesh_link_set_passive(false);
    advance(200);
    node_read();
    CHECK(packets == 4);
    CHECK(mesh_link_stats()->texts_tx == 4);

    // Replies carry reply_id.
    link_up();
    node_read();
    CHECK(mesh_link_send_text(MESH_CONV_CHANNEL(0), "+1", 2, 1234, &m) == MESH_SEND_OK);
    node_read();
    CHECK(last_packet.decoded.reply_id == 1234 && m->reply_id == 1234);

    // Our own node's other client sent something: shows up as ours.
    deliver(fr_text(ME, MESH_ADDR_BROADCAST, 0, 300, "from the node itself"));
    m = (mesh_msg_t *)mesh_msgs_recent(MESH_CONV_CHANNEL(0), 0);
    CHECK(m && m->status == MESH_MSG_SENT && m->from == ME);
    CHECK(mesh_msgs_unread_total() == 0);

    // A node reboot loses its queue: anything in flight fails.
    mesh_msg_t *pending = send_text(MESH_CONV_CHANNEL(1), "lost");
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_rebooted_tag;
    fr.rebooted = true;
    deliver(fr);
    CHECK(pending->status == MESH_MSG_FAILED);
    CHECK(m->status == MESH_MSG_FAILED);
    node_read();
}

static void test_commands(void) {
    link_up();
    node_read();
    char line[128];

    // Someone else's command comes back whole, arguments and all.
    strcpy(line, "bl 64");
    CHECK(!mesh_cmd_run(line));
    CHECK(strcmp(line, "bl 64") == 0);
    strcpy(line, "keys");
    CHECK(!mesh_cmd_run(line));
    CHECK(strcmp(line, "keys") == 0);

    strcpy(line, "send 1   hello  there ");
    CHECK(mesh_cmd_run(line));
    node_read();
    CHECK(last_packet.channel == 1 && last_packet.to == MESH_ADDR_BROADCAST);
    CHECK(last_packet.decoded.payload.size == 12 &&
          memcmp(last_packet.decoded.payload.bytes, "hello  there", 12) == 0);

    strcpy(line, "dm hill yo");
    CHECK(mesh_cmd_run(line));
    node_read();
    CHECK(last_packet.to == 0x11111111);

    strcpy(line, "dm !33333333 hey");
    CHECK(mesh_cmd_run(line));
    node_read();
    CHECK(last_packet.to == 0x33333333);

    // Short names can start with digits
    deliver(fr_node(0x44444444, "Twelve", "12Tc", 1700000000));
    strcpy(line, "dm 12tc hi");
    CHECK(mesh_cmd_run(line));
    node_read();
    CHECK(last_packet.to == 0x44444444);
    strcpy(line, "telreq 12Tc local");
    CHECK(mesh_cmd_run(line));
    node_read();
    CHECK(last_packet.to == 0x44444444 &&
          last_packet.decoded.portnum == meshtastic_PortNum_TELEMETRY_APP);

    packets = 0;
    strcpy(line, "dm nobody hey");
    CHECK(mesh_cmd_run(line));
    strcpy(line, "send 2 disabled");
    CHECK(mesh_cmd_run(line));
    strcpy(line, "send");
    CHECK(mesh_cmd_run(line));
    node_read();
    CHECK(packets == 0);

    strcpy(line, "msgs");
    CHECK(mesh_cmd_run(line));
    strcpy(line, "msgs mob 5");
    CHECK(mesh_cmd_run(line));
    strcpy(line, "desktop on");
    CHECK(!mesh_cmd_run(line));
}

// ---- node settings ---------------------------------------------------------------

// The rest of a config dump: sections, a module, and our own User in full
static void dump_settings(uint32_t region, uint32_t hops, const char *lname) {
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_config_tag;
    fr.config.which_payload_variant = meshtastic_Config_lora_tag;
    meshtastic_Config_LoRaConfig *l = &fr.config.payload_variant.lora;
    l->use_preset = true;
    l->region = (meshtastic_Config_LoRaConfig_RegionCode)region;
    l->hop_limit = hops;
    l->tx_enabled = true;
    l->ignore_incoming_count = 1; // not shown in the UI: must survive a write
    l->ignore_incoming[0] = 0xdeadbeef;
    emit(&fr);
    fr = (meshtastic_FromRadio)meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_config_tag;
    fr.config.which_payload_variant = meshtastic_Config_device_tag;
    fr.config.payload_variant.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    emit(&fr);
    fr = (meshtastic_FromRadio)meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_moduleConfig_tag;
    fr.moduleConfig.which_payload_variant = meshtastic_ModuleConfig_mqtt_tag;
    fr.moduleConfig.payload_variant.mqtt.enabled = true;
    emit(&fr);
    fr = fr_node(ME, lname, "CALC", 1700000000);
    fr.node_info.user.is_licensed = true;
    emit(&fr);
}

static void resync(uint32_t region, uint32_t hops, const char *lname) {
    advance(200);
    uint32_t nonce = node_read();
    CHECK(nonce);
    meshtastic_FromRadio fr = fr_my_info(ME, 3);
    emit(&fr);
    dump_settings(region, hops, lname);
    fr = fr_channel(0, meshtastic_Channel_Role_PRIMARY, "", 1);
    emit(&fr);
    fr = fr_complete(nonce);
    emit(&fr);
    flush_stream();
    CHECK(mesh_link_state() == MESH_LINK_READY);
}

static void test_admin(void) {
    link_up();
    // Settings arrive in a second dump (link_up's has none)
    mesh_link_request_config();
    resync(meshtastic_Config_LoRaConfig_RegionCode_US, 3, "Calcmesh Base");
    CHECK(mesh_db.has_owner && strcmp(mesh_db.owner.long_name, "Calcmesh Base") == 0);
    CHECK(mesh_db_config(meshtastic_Config_lora_tag));
    CHECK(!mesh_db_config(meshtastic_Config_power_tag));
    const meshtastic_ModuleConfig *mq = mesh_db_module(meshtastic_ModuleConfig_mqtt_tag);
    CHECK(mq && mq->payload_variant.mqtt.enabled);

    mesh_edit_poll(); // takes the fresh settings
    CHECK(mesh_edit_available() == (MESH_EDIT_OWNER | MESH_EDIT_DEVICE | MESH_EDIT_LORA |
                                    MESH_EDIT_CHANNEL(0)));
    CHECK(mesh_edit_dirty() == 0);
    CHECK(!mesh_edit_write()); // nothing to write
    // Power never came: edits there don't count
    mesh_edit.power.is_power_saving = true;
    CHECK(mesh_edit_dirty() == 0);

    mesh_edit.lora.hop_limit = 5;
    strcpy(mesh_edit.owner.long_name, "Calc 12Tc");
    CHECK(mesh_edit_dirty() == (MESH_EDIT_OWNER | MESH_EDIT_LORA));
    node_read();
    n_admin = 0;
    CHECK(mesh_edit_write());
    node_read();
    CHECK(n_admin == 4);
    CHECK(admins[0].which_payload_variant == meshtastic_AdminMessage_begin_edit_settings_tag);
    CHECK(admins[1].which_payload_variant == meshtastic_AdminMessage_set_owner_tag);
    CHECK(strcmp(admins[1].set_owner.long_name, "Calc 12Tc") == 0);
    CHECK(strcmp(admins[1].set_owner.short_name, "CALC") == 0);
    CHECK(admins[1].set_owner.is_licensed); // kept as the node had it
    CHECK(admins[2].which_payload_variant == meshtastic_AdminMessage_set_config_tag);
    CHECK(admins[2].set_config.which_payload_variant == meshtastic_Config_lora_tag);
    CHECK(admins[2].set_config.payload_variant.lora.hop_limit == 5);
    CHECK(admins[2].set_config.payload_variant.lora.region ==
          meshtastic_Config_LoRaConfig_RegionCode_US);
    CHECK(admins[2].set_config.payload_variant.lora.ignore_incoming_count == 1);
    CHECK(admins[3].which_payload_variant == meshtastic_AdminMessage_commit_edit_settings_tag);
    for (int i = 0; i < n_admin; i++) {
        // From our client (0) to our node, on the admin port
        CHECK(admin_packets[i].from == 0 && admin_packets[i].to == ME);
        CHECK(admin_packets[i].decoded.portnum == meshtastic_PortNum_ADMIN_APP);
    }
    CHECK(mesh_edit_state() == MESH_EDIT_RESTARTING);

    // It restarts and says so; the link resyncs, and the settings read back
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_rebooted_tag;
    fr.rebooted = true;
    deliver(fr);
    mesh_edit_poll();
    CHECK(mesh_edit_state() == MESH_EDIT_RESTARTING);
    resync(meshtastic_Config_LoRaConfig_RegionCode_US, 5, "Calc 12Tc");
    mesh_edit_poll();
    CHECK(mesh_edit_state() == MESH_EDIT_DONE);
    CHECK(mesh_edit_base.lora.hop_limit == 5 && mesh_edit_dirty() == 0);
    mesh_edit_ack();
    CHECK(mesh_edit_state() == MESH_EDIT_IDLE);

    // One that doesn't stick, from a node that restarts without saying so:
    // after a while we ask for the config ourselves
    mesh_edit.lora.region = meshtastic_Config_LoRaConfig_RegionCode_EU_868;
    CHECK(mesh_edit_write());
    advance(MESH_EDIT_RESYNC_MS - 1000);
    mesh_edit_poll();
    CHECK(mesh_edit_state() == MESH_EDIT_RESTARTING);
    advance(1000);
    mesh_edit_poll();
    mesh_link_poll();
    resync(meshtastic_Config_LoRaConfig_RegionCode_US, 5, "Calc 12Tc");
    mesh_edit_poll();
    CHECK(mesh_edit_state() == MESH_EDIT_MISMATCH);
    CHECK(mesh_edit_mismatch() == MESH_EDIT_LORA);
    CHECK(mesh_edit.lora.region == meshtastic_Config_LoRaConfig_RegionCode_US);

    // Refused: a routing error for one of the admin packets
    mesh_edit.lora.hop_limit = 2;
    node_read();
    n_admin = 0;
    CHECK(mesh_edit_write());
    node_read();
    deliver(fr_routing(ME, admin_packets[1].id, meshtastic_Routing_Error_NOT_AUTHORIZED));
    mesh_edit_poll();
    CHECK(mesh_edit_state() == MESH_EDIT_REFUSED);
    CHECK(mesh_link_stats()->admin_last_error == meshtastic_Routing_Error_NOT_AUTHORIZED);

    // A node that never comes back
    mesh_edit.lora.hop_limit = 2;
    CHECK(mesh_edit_write());
    for (int i = 0; i < MESH_EDIT_TIMEOUT_MS / 1000 + 1; i++) {
        advance(1000);
        mesh_link_poll();
        mesh_edit_poll();
        node_read();
    }
    CHECK(mesh_edit_state() == MESH_EDIT_NO_RESTART);

    // Not while desktop mode has the node
    mesh_edit_ack();
    mesh_link_set_passive(true);
    mesh_edit.lora.hop_limit = 2;
    CHECK(!mesh_edit_write());
    meshtastic_AdminMessage a = meshtastic_AdminMessage_init_zero;
    CHECK(!mesh_link_send_admin(&a));
}

// ---- remote admin ----------------------------------------------------------------

static const uint32_t HILL = 0x11111111;

// An admin answer from another node, for request id
static meshtastic_FromRadio fr_admin(uint32_t from, uint32_t request_id,
                                     const meshtastic_AdminMessage *a) {
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_packet_tag;
    meshtastic_MeshPacket *p = &fr.packet;
    p->from = from;
    p->to = ME;
    p->id = request_id ^ 0x3333;
    p->pki_encrypted = true;
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = meshtastic_PortNum_ADMIN_APP;
    p->decoded.request_id = request_id;
    pb_ostream_t os = pb_ostream_from_buffer(p->decoded.payload.bytes,
                                             sizeof p->decoded.payload.bytes);
    CHECK(pb_encode(&os, meshtastic_AdminMessage_fields, a));
    p->decoded.payload.size = (pb_size_t)os.bytes_written;
    return fr;
}

// HILL's settings, as it would answer
static meshtastic_User hill_owner;
static meshtastic_Config_LoRaConfig hill_lora;
static uint8_t hill_key = 0x40; // its session key's bytes; changes on restart

// The one admin message the link has sent since the last call; answer it as
// HILL would. Returns the request (0 if none was sent).
static pb_size_t hill_answer(void) {
    n_admin = 0;
    node_read();
    CHECK(n_admin <= 1); // one at a time
    if (!n_admin)
        return 0;
    const meshtastic_AdminMessage *q = &admins[0];
    const meshtastic_MeshPacket *qp = &admin_packets[0];
    meshtastic_AdminMessage r = meshtastic_AdminMessage_init_zero;
    r.session_passkey.size = 8;
    memset(r.session_passkey.bytes, hill_key, 8);
    switch (q->which_payload_variant) {
    case meshtastic_AdminMessage_get_device_metadata_request_tag:
        r.which_payload_variant = meshtastic_AdminMessage_get_device_metadata_response_tag;
        strcpy(r.get_device_metadata_response.firmware_version, "2.7.15.567b8ea");
        break;
    case meshtastic_AdminMessage_get_owner_request_tag:
        r.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
        r.get_owner_response = hill_owner;
        break;
    case meshtastic_AdminMessage_get_config_request_tag:
        r.which_payload_variant = meshtastic_AdminMessage_get_config_response_tag;
        r.get_config_response.which_payload_variant = (pb_size_t)(q->get_config_request + 1);
        if (r.get_config_response.which_payload_variant == meshtastic_Config_lora_tag)
            r.get_config_response.payload_variant.lora = hill_lora;
        break;
    case meshtastic_AdminMessage_get_channel_request_tag: {
        r.which_payload_variant = meshtastic_AdminMessage_get_channel_response_tag;
        int idx = (int)q->get_channel_request - 1;
        r.get_channel_response.index = idx;
        r.get_channel_response.role = idx == 0   ? meshtastic_Channel_Role_PRIMARY
                                      : idx == 1 ? meshtastic_Channel_Role_SECONDARY
                                                 : meshtastic_Channel_Role_DISABLED;
        r.get_channel_response.has_settings = true;
        strcpy(r.get_channel_response.settings.name, idx == 1 ? "Ops" : "");
        break;
    }
    default:
        // A change: it needs the session key
        CHECK(q->session_passkey.size == 8 && q->session_passkey.bytes[0] == hill_key);
        if (q->which_payload_variant == meshtastic_AdminMessage_set_owner_tag)
            hill_owner = q->set_owner;
        if (q->which_payload_variant == meshtastic_AdminMessage_set_config_tag &&
            q->set_config.which_payload_variant == meshtastic_Config_lora_tag)
            hill_lora = q->set_config.payload_variant.lora;
        if (q->which_payload_variant == meshtastic_AdminMessage_commit_edit_settings_tag)
            hill_key++; // it restarts
        CHECK(!qp->decoded.want_response);
        // A neighbour's rebroadcast (an implicit ack) first: not enough
        deliver(fr_routing(0x33333333, qp->id, meshtastic_Routing_Error_NONE));
        deliver(fr_routing(HILL, qp->id, meshtastic_Routing_Error_NONE));
        return q->which_payload_variant;
    }
    CHECK(qp->decoded.want_response);
    deliver(fr_admin(HILL, qp->id, &r));
    return q->which_payload_variant;
}

static void test_remote_admin(void) {
    link_up();
    mesh_edit_select(0);
    memset(&hill_owner, 0, sizeof hill_owner);
    strcpy(hill_owner.long_name, "Hilltop Router");
    strcpy(hill_owner.short_name, "HILL");
    hill_owner.public_key.size = 32;
    memset(hill_owner.public_key.bytes, 7, 32);
    memset(&hill_lora, 0, sizeof hill_lora);
    hill_lora.use_preset = true;
    hill_lora.region = meshtastic_Config_LoRaConfig_RegionCode_US;
    hill_lora.hop_limit = 3;
    hill_lora.tx_enabled = true;

    // Pointed at HILL: nothing known until fetched, and nothing sent yet
    node_read();
    CHECK(mesh_edit_select(HILL));
    CHECK(mesh_edit_target() == HILL);
    CHECK(mesh_edit_available() == 0);
    n_admin = 0;
    node_read();
    CHECK(n_admin == 0);

    CHECK(mesh_edit_fetch());
    CHECK(!mesh_edit_fetch()); // already at it
    CHECK(ra_state() == RA_BUSY);
    CHECK(!mesh_edit_select(0)); // not mid-fetch
    // Each goes out PKI, acked, wanting an answer, at our hop limit (the
    // default here: link_up sends no LoRa config)
    int answered = 0;
    while (hill_answer()) {
        const meshtastic_MeshPacket *qp = &admin_packets[0];
        CHECK(qp->to == HILL && qp->from == 0);
        CHECK(qp->pki_encrypted && qp->want_ack && qp->hop_limit == 3);
        answered++;
        mesh_edit_poll();
    }
    // metadata, owner, 5 configs, channels 0-2 (2 is disabled: no more)
    CHECK(answered == 10);
    CHECK(ra_state() == RA_DONE);
    CHECK(strcmp(ra.firmware, "2.7.15.567b8ea") == 0);
    CHECK(mesh_edit_available() == (MESH_EDIT_OWNER | MESH_EDIT_DEVICE | MESH_EDIT_POSITION |
                                    MESH_EDIT_POWER | MESH_EDIT_LORA | MESH_EDIT_BLUETOOTH |
                                    MESH_EDIT_CHANNELS));
    CHECK(strcmp(mesh_edit_base.owner.long_name, "Hilltop Router") == 0);
    CHECK(mesh_edit_base.lora.hop_limit == 3);
    CHECK(strcmp(mesh_edit_base.channels[1].settings.name, "Ops") == 0);
    CHECK(mesh_edit_base.channels[5].role == meshtastic_Channel_Role_DISABLED);
    CHECK(mesh_edit_dirty() == 0);

    // A write: fresh key, begin, owner, lora, commit; wait; read back
    strcpy(mesh_edit.owner.long_name, "Hilltop 2");
    mesh_edit.lora.hop_limit = 4;
    CHECK(mesh_edit_dirty() == (MESH_EDIT_OWNER | MESH_EDIT_LORA));
    CHECK(mesh_edit_write());
    CHECK(mesh_edit_state() == MESH_EDIT_RESTARTING);
    static const pb_size_t want[] = {
        meshtastic_AdminMessage_get_owner_request_tag,
        meshtastic_AdminMessage_begin_edit_settings_tag,
        meshtastic_AdminMessage_set_owner_tag,
        meshtastic_AdminMessage_set_config_tag,
        meshtastic_AdminMessage_commit_edit_settings_tag,
    };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        CHECK(hill_answer() == want[i]);
        mesh_edit_poll();
    }
    CHECK(ra_written());
    CHECK(hill_answer() == 0); // waiting for it to restart
    advance(RA_RESTART_MS);
    mesh_edit_poll();
    CHECK(hill_answer() == meshtastic_AdminMessage_get_owner_request_tag);
    mesh_edit_poll();
    CHECK(hill_answer() == meshtastic_AdminMessage_get_config_request_tag);
    mesh_edit_poll();
    CHECK(mesh_edit_state() == MESH_EDIT_DONE);
    CHECK(strcmp(mesh_edit_base.owner.long_name, "Hilltop 2") == 0);
    CHECK(mesh_edit_base.lora.hop_limit == 4 && mesh_edit_dirty() == 0);
    CHECK(hill_owner.public_key.size == 32); // written back whole
    mesh_edit_ack();

    // Refused: our key isn't one of its admin keys. Nothing gets begun, so
    // nothing needs committing; the edits stay to try again.
    mesh_edit.lora.hop_limit = 6;
    CHECK(mesh_edit_write());
    n_admin = 0;
    node_read();
    CHECK(n_admin == 1 && admins[0].which_payload_variant == meshtastic_AdminMessage_get_owner_request_tag);
    deliver(fr_routing(HILL, admin_packets[0].id, meshtastic_Routing_Error_ADMIN_PUBLIC_KEY_UNAUTHORIZED));
    mesh_edit_poll();
    CHECK(mesh_edit_state() == MESH_EDIT_REFUSED);
    CHECK(mesh_edit_error() == meshtastic_Routing_Error_ADMIN_PUBLIC_KEY_UNAUTHORIZED);
    CHECK(mesh_edit.lora.hop_limit == 6 && mesh_edit_dirty() == MESH_EDIT_LORA);
    n_admin = 0;
    node_read();
    CHECK(n_admin == 0);

    // Lost partway: the open transaction still gets its commit
    mesh_edit_ack();
    CHECK(mesh_edit_write());
    CHECK(hill_answer() == meshtastic_AdminMessage_get_owner_request_tag);
    mesh_edit_poll();
    CHECK(hill_answer() == meshtastic_AdminMessage_begin_edit_settings_tag);
    mesh_edit_poll();
    n_admin = 0;
    node_read();
    CHECK(n_admin == 1 && admins[0].which_payload_variant == meshtastic_AdminMessage_set_config_tag);
    // Our node gives up on it
    deliver(fr_routing(ME, admin_packets[0].id, meshtastic_Routing_Error_MAX_RETRANSMIT));
    mesh_edit_poll();
    CHECK(mesh_edit_state() == MESH_EDIT_RESTARTING);
    CHECK(hill_answer() == meshtastic_AdminMessage_commit_edit_settings_tag);
    mesh_edit_poll();
    CHECK(mesh_edit_state() == MESH_EDIT_NO_RESTART);
    CHECK(mesh_edit_error() == meshtastic_Routing_Error_MAX_RETRANSMIT);
    CHECK(hill_answer() == 0); // and that's the end of it

    // No answer at all
    mesh_edit_ack();
    CHECK(mesh_edit_fetch());
    node_read();
    for (int i = 0; i < RA_STEP_MS / 1000 + 1; i++) {
        advance(1000);
        mesh_edit_poll();
    }
    CHECK(ra_state() == RA_FAILED && ra_error() == RA_ERR_NO_ANSWER);
    CHECK(mesh_edit_state() == MESH_EDIT_IDLE); // a fetch, not a write

    // An answer from someone else, or to something else, doesn't count
    CHECK(mesh_edit_fetch());
    n_admin = 0;
    node_read();
    meshtastic_AdminMessage r = meshtastic_AdminMessage_init_zero;
    r.which_payload_variant = meshtastic_AdminMessage_get_device_metadata_response_tag;
    deliver(fr_admin(0x33333333, admin_packets[0].id, &r));
    deliver(fr_admin(HILL, admin_packets[0].id + 1, &r));
    mesh_edit_poll();
    CHECK(ra_done() == 0 && ra_state() == RA_BUSY);
    deliver(fr_admin(HILL, admin_packets[0].id, &r));
    CHECK(ra_done() == 1);

    // Back to our own node, once it's done
    for (int i = 0; i < RA_STEP_MS / 1000 + 1; i++) {
        advance(1000);
        mesh_edit_poll();
    }
    CHECK(mesh_edit_select(0));
    CHECK(mesh_edit_target() == 0);
    CHECK(strcmp(mesh_edit_base.owner.long_name, "Calcmesh Base") == 0);
}

static meshtastic_FromRadio fr_telemetry(uint32_t from, uint32_t request_id, uint32_t rx_time,
                                         const meshtastic_Telemetry *t) {
    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    fr.which_payload_variant = meshtastic_FromRadio_packet_tag;
    meshtastic_MeshPacket *p = &fr.packet;
    p->from = from;
    p->to = request_id ? ME : MESH_ADDR_BROADCAST;
    p->id = 0x4000 + rx_time % 1000;
    p->rx_time = rx_time;
    p->has_rx_time = rx_time != 0;
    p->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum = meshtastic_PortNum_TELEMETRY_APP;
    p->decoded.request_id = request_id;
    pb_ostream_t os = pb_ostream_from_buffer(p->decoded.payload.bytes,
                                             sizeof p->decoded.payload.bytes);
    CHECK(pb_encode(&os, meshtastic_Telemetry_fields, t));
    p->decoded.payload.size = (pb_size_t)os.bytes_written;
    return fr;
}

static meshtastic_Telemetry device(uint32_t batt, float volts, float chutil) {
    meshtastic_Telemetry t = meshtastic_Telemetry_init_zero;
    t.which_variant = meshtastic_Telemetry_device_metrics_tag;
    meshtastic_DeviceMetrics *d = &t.variant.device_metrics;
    d->has_battery_level = d->has_voltage = d->has_channel_utilization = true;
    d->battery_level = batt;
    d->voltage = volts;
    d->channel_utilization = chutil;
    return t;
}

// The telemetry request the link sent last
static meshtastic_Telemetry sent_request(void) {
    meshtastic_Telemetry t = meshtastic_Telemetry_init_zero;
    node_read();
    CHECK(last_packet.decoded.portnum == meshtastic_PortNum_TELEMETRY_APP);
    pb_istream_t is = pb_istream_from_buffer(last_packet.decoded.payload.bytes,
                                             last_packet.decoded.payload.size);
    CHECK(pb_decode(&is, meshtastic_Telemetry_fields, &t));
    return t;
}

static void test_metrics(void) {
    link_up();
    mesh_metrics_clear();
    node_read();

    // A broadcast from the hill: stored, a history point, and the Nodes
    // tab's battery follows
    meshtastic_Telemetry t = device(55, 3.81f, 12.5f);
    deliver(fr_telemetry(0x11111111, 0, 1700001000, &t));
    const mesh_metrics_t *e = mesh_metrics_find(0x11111111);
    CHECK(e && e->have[MET_DEVICE] && !e->have[MET_LOCAL]);
    CHECK(e && e->time[MET_DEVICE] == 1700001000 && e->device.battery_level == 55);
    CHECK(e && e->nhist == 1 && e->hist[0].mv == 3810 && e->hist[0].chutil == 25 &&
          e->hist[0].batt == 55 && e->hist[0].temp == MET_NONE_TEMP);
    CHECK(mesh_db_find(0x11111111)->battery == 55);

    // Environment a few seconds later shares the point; device again within
    // the gap doesn't add one; after it, it does
    meshtastic_Telemetry env = meshtastic_Telemetry_init_zero;
    env.which_variant = meshtastic_Telemetry_environment_metrics_tag;
    env.variant.environment_metrics.has_temperature = true;
    env.variant.environment_metrics.temperature = -3.46f;
    deliver(fr_telemetry(0x11111111, 0, 1700001005, &env));
    CHECK(e->nhist == 1 && e->hist[0].temp == -35 && e->env.has_temperature);
    deliver(fr_telemetry(0x11111111, 0, 1700001060, &t));
    CHECK(e->nhist == 1);
    t = device(54, 3.80f, 9);
    deliver(fr_telemetry(0x11111111, 0, 1700001000 + MESH_METRICS_GAP_S, &t));
    CHECK(e->nhist == 2 && mesh_metrics_point(e, 1)->mv == 3800);
    CHECK(mesh_metrics_point(e, 1)->temp == MET_NONE_TEMP);
    // A clock that went backwards keeps the numbers but adds no point
    deliver(fr_telemetry(0x11111111, 0, 1600000000, &t));
    CHECK(e->nhist == 2 && e->time[MET_DEVICE] == 1600000000);

    // The history is a ring
    for (int i = 0; i < MESH_METRICS_HIST + 5; i++) {
        t = device(50, 3.0f + i / 100.0f, 1);
        deliver(fr_telemetry(0x33333333, 0, 1700100000 + (uint32_t)i * MESH_METRICS_GAP_S, &t));
    }
    const mesh_metrics_t *mob = mesh_metrics_find(0x33333333);
    CHECK(mob && mob->nhist == MESH_METRICS_HIST);
    CHECK(mob && mesh_metrics_point(mob, 0)->mv == 3050);
    CHECK(mob && mesh_metrics_point(mob, MESH_METRICS_HIST - 1)->mv ==
                     3000 + (MESH_METRICS_HIST + 4) * 10);

    // Asking the hill for local stats: one packet, want_response, the
    // variant present and empty; over the air at the hop limit
    CHECK(mesh_metrics_request(0x11111111, MET_LOCAL));
    meshtastic_Telemetry req = sent_request();
    CHECK(req.which_variant == meshtastic_Telemetry_local_stats_tag);
    CHECK(last_packet.to == 0x11111111 && last_packet.decoded.want_response);
    CHECK(last_packet.hop_limit == 3 && !last_packet.want_ack && last_packet.channel == 0);
    CHECK(mesh_metrics_req_state() == MET_REQ_WAITING);
    uint32_t id = last_packet.id;
    // Someone else's answer doesn't count
    meshtastic_Telemetry ls = meshtastic_Telemetry_init_zero;
    ls.which_variant = meshtastic_Telemetry_local_stats_tag;
    ls.variant.local_stats.num_packets_tx = 77;
    ls.variant.local_stats.noise_floor = -104;
    deliver(fr_telemetry(0x11111111, id + 1, 1700002000, &ls));
    CHECK(mesh_metrics_req_state() == MET_REQ_WAITING);
    deliver(fr_telemetry(0x11111111, id, 1700002001, &ls));
    CHECK(mesh_metrics_req_state() == MET_REQ_DONE);
    CHECK(e->have[MET_LOCAL] && e->local.num_packets_tx == 77 && e->local.noise_floor == -104);

    // No sensor: the node says so with a routing error
    CHECK(mesh_metrics_request(0x11111111, MET_POWER));
    id = sent_request().which_variant == meshtastic_Telemetry_power_metrics_tag ? last_packet.id : 0;
    CHECK(id != 0);
    deliver(fr_routing(0x11111111, id, meshtastic_Routing_Error_NO_RESPONSE));
    CHECK(mesh_metrics_req_state() == MET_REQ_FAILED);
    CHECK(mesh_metrics_req_error() == meshtastic_Routing_Error_NO_RESPONSE);

    // Silence: given up on after its time
    CHECK(mesh_metrics_request(0x11111111, MET_ENV));
    advance(MET_REQ_REMOTE_MS - 100);
    CHECK(mesh_metrics_req_state() == MET_REQ_WAITING);
    mesh_metrics_poll();
    advance(200);
    mesh_metrics_poll();
    CHECK(mesh_metrics_req_state() == MET_REQ_FAILED);
    CHECK(mesh_metrics_req_error() == MET_ERR_NO_ANSWER);

    // Our own node: never on the air
    CHECK(mesh_metrics_request(ME, MET_DEVICE));
    req = sent_request();
    CHECK(last_packet.to == ME && last_packet.hop_limit == 0);
    CHECK(req.which_variant == meshtastic_Telemetry_device_metrics_tag);

    // The table keeps our node when it's full, and drops the stalest
    t = device(90, 4.0f, 5);
    deliver(fr_telemetry(ME, 0, 1600000000, &t));
    for (int i = 0; i < MESH_METRICS_NODES; i++)
        deliver(fr_telemetry(0x50000000 + (uint32_t)i, 0, 1700200000 + (uint32_t)i, &t));
    CHECK(mesh_metrics_count() == MESH_METRICS_NODES);
    CHECK(mesh_metrics_find(ME) != NULL);
    CHECK(mesh_metrics_find(0x50000000 + MESH_METRICS_NODES - 1) != NULL);
    CHECK(mesh_metrics_find(0x11111111) == NULL); // its newest was 1700002001

    // And it all survives a resync with the node
    mesh_link_request_config();
    advance(200);
    uint32_t nonce = node_read();
    meshtastic_FromRadio fr = fr_my_info(ME, 1);
    emit(&fr);
    fr = fr_complete(nonce);
    emit(&fr);
    flush_stream();
    CHECK(mesh_metrics_find(ME) && mesh_metrics_count() == MESH_METRICS_NODES);

    // Not ready: no request
    mesh_link_set_passive(true);
    CHECK(!mesh_metrics_request(ME, MET_DEVICE));
    CHECK(mesh_metrics_req_error() == MET_ERR_NO_LINK);
    mesh_link_set_passive(false);
}

int main(void) {
    test_frame_resync();
    test_noise();
    test_handshake();
    test_console();
    test_eviction();
    test_passive();
    test_receive();
    test_position();
    test_send();
    test_commands();
    test_admin();
    test_remote_admin();
    test_metrics();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
