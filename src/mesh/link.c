#include "link.h"

#include <stdio.h>
#include <string.h>

#include "frame.h"
#include "nodedb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include "meshtastic/mesh.pb.h"

// The Python client sends this many START2 bytes and waits before its first
// frame, so a node whose serial port is idle or asleep resyncs.
#define WAKE_BYTES 32
#define WAKE_SETTLE_MS 100
// Re-ask if the dump hasn't finished this long after the last byte arrived.
#define CONFIG_STALL_MS 5000
// The node drops a serial client it hasn't heard from in a while; the
// Python client heartbeats every 300 s.
#define HEARTBEAT_MS 300000

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
static bool db_cleared;
static void (*on_ready)(void) = mesh_db_dump;

// Decode target lives in .bss: FromRadio is ~770 bytes and the core 0 stack
// is 2 KiB by default.
static meshtastic_FromRadio from_radio;

// Console text from the node, collected into lines.
static char console[128];
static size_t console_len;

static uint32_t now(void) { return io.now_ms(io.ctx); }

static void set_state(mesh_link_state_t s) {
    state = s;
    state_since_ms = now();
}

// Outgoing message and its encoding, static for the same reason.
static meshtastic_ToRadio to_radio;
static uint8_t tx_buf[FRAME_HEADER_LEN + meshtastic_ToRadio_size];

static void send_to_radio(void) {
    uint8_t *buf = tx_buf;
    pb_ostream_t os =
        pb_ostream_from_buffer(buf + FRAME_HEADER_LEN, sizeof tx_buf - FRAME_HEADER_LEN);
    if (!pb_encode(&os, meshtastic_ToRadio_fields, &to_radio)) {
        printf("[link] ToRadio encode failed: %s\n", PB_GET_ERROR(&os));
        return;
    }
    frame_header(buf, os.bytes_written);
    io.write(buf, FRAME_HEADER_LEN + os.bytes_written, io.ctx);
    last_tx_ms = now();
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
    printf("[link] want_config sent, nonce %lu\n", (unsigned long)nonce);
    set_state(MESH_LINK_CONFIGURING);
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
    console_len = 0;
    nonce = io.seed ^ now();
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

mesh_link_state_t mesh_link_state(void) { return state; }
const mesh_link_stats_t *mesh_link_stats(void) { return &stats; }
void mesh_link_on_ready(void (*cb)(void)) { on_ready = cb; }

static void console_byte(uint8_t b) {
    if (b == '\n' || b == '\r' || console_len == sizeof console - 1) {
        if (console_len) {
            console[console_len] = '\0';
            printf("[node] %s\n", console);
            console_len = 0;
        }
        if (b == '\n' || b == '\r')
            return;
    }
    // Keep printable ASCII and UTF-8 bytes; escape-code junk is dropped.
    if (b >= 0x20 && b != 0x7f)
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
// leaves the previous picture intact.
static void begin_dump(void) {
    if (state == MESH_LINK_CONFIGURING && !db_cleared) {
        mesh_db_clear();
        db_cleared = true;
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
        break;
    case meshtastic_FromRadio_channel_tag:
        begin_dump();
        mesh_db_set_channel(&fr->channel);
        break;
    case meshtastic_FromRadio_config_tag:
        begin_dump();
        mesh_db_set_config(&fr->config);
        break;
    case meshtastic_FromRadio_config_complete_id_tag:
        if (state != MESH_LINK_CONFIGURING || fr->config_complete_id != nonce) {
            printf("[link] ignoring stale config_complete %lu\n",
                   (unsigned long)fr->config_complete_id);
            break;
        }
        printf("[link] config complete: %u nodes, %lu frames\n",
               mesh_db.node_count, (unsigned long)stats.frames);
        set_state(MESH_LINK_READY);
        if (on_ready)
            on_ready();
        break;
    case meshtastic_FromRadio_rebooted_tag:
        printf("[link] node rebooted, re-requesting config\n");
        mesh_link_request_config();
        break;
    case meshtastic_FromRadio_log_record_tag:
        printf("[node %s] %s%s%s\n", log_level(fr->log_record.level),
               fr->log_record.source, fr->log_record.source[0] ? ": " : "",
               fr->log_record.message);
        break;
    case meshtastic_FromRadio_packet_tag: {
        const meshtastic_MeshPacket *p = &fr->packet;
        printf("[link] packet !%08lx -> !%08lx ch %u", (unsigned long)p->from,
               (unsigned long)p->to, p->channel);
        if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag)
            printf(" port %d, %u bytes\n", p->decoded.portnum,
                   (unsigned)p->decoded.payload.size);
        else
            printf(" (encrypted)\n");
        break;
    }
    case meshtastic_FromRadio_clientNotification_tag:
        printf("[node notice] %s\n", fr->clientNotification.message);
        break;
    default:
        // moduleConfig, queueStatus, fileInfo, deviceuiConfig, region_presets
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
    if (len)
        last_rx_ms = now();
    for (size_t i = 0; i < len; i++) {
        switch (frame_rx_push(&rx, data[i])) {
        case FRAME_RX_READY:
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
            printf("[link] no config_complete, retrying\n");
            mesh_link_request_config();
        }
        break;
    }
    case MESH_LINK_READY:
        if (t - last_tx_ms >= HEARTBEAT_MS)
            send_heartbeat();
        break;
    default:
        break;
    }
}
