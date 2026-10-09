// Drives the link against a scripted fake node: the node side decodes what the
// link writes and answers with framed FromRadio records.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame.h"
#include "link.h"
#include "nodedb.h"
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
// want_config nonce seen (0 if none) and count heartbeats.
static int heartbeats, wake_bytes;
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
    CHECK(ready_calls == 1);
    CHECK(mesh_link_stats()->bad_frames == 0);
    CHECK(mesh_db.my_node_num == me);
    CHECK(strcmp(mesh_db.pio_env, "heltec-v4") == 0);
    CHECK(strcmp(mesh_db_channel_name(0), "MediumFast") == 0);
    CHECK(strcmp(mesh_db_channel_name(1), "Ops") == 0);
    CHECK(mesh_db.channels[1].psk_len == 32);
    CHECK(mesh_db.channels[2].role == meshtastic_Channel_Role_DISABLED);

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
    advance(200);
    uint32_t nonce2 = node_read();
    CHECK(nonce2 != 0 && nonce2 != nonce);
    CHECK(mesh_db.node_count > 0);

    // Node goes silent: link retries after the stall timeout.
    advance(5200);
    CHECK(node_read() != 0);
    CHECK(mesh_link_stats()->requests == 3);
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

int main(void) {
    test_frame_resync();
    test_noise();
    test_handshake();
    test_eviction();
    test_passive();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
