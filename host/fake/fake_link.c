// Host-only stand-in for link.c: the UI is tested against the real message
// store (msgs.c) but no node, so the link's state, sending and clock are
// whatever the test sets.
#include <string.h>

#include "link.h"
#include "meshtastic/admin.pb.h"
#include "nodedb.h"
#include "scene.h"

mesh_link_state_t fake_link_state = MESH_LINK_READY;
mesh_send_result_t fake_send_result = MESH_SEND_OK;
uint32_t fake_epoch;

bool fake_node_silent;
mesh_link_stats_t fake_link_stats;
uint32_t fake_ms;
int fake_admin_sent;
int fake_admin_last;

const mesh_link_stats_t *mesh_link_stats(void) { return &fake_link_stats; }
uint32_t mesh_link_ms(void) { return fake_ms; }
void mesh_link_request_config(void) { fake_link_state = MESH_LINK_CONFIGURING; }

bool mesh_link_send_admin(const struct _meshtastic_AdminMessage *a) {
    if (fake_link_state != MESH_LINK_READY)
        return false;
    fake_admin_sent++;
    fake_admin_last = a->which_payload_variant;
    return true;
}

// Remote admin: one message waits at a time; fake_remote_answer() has the
// fake remote node answer it.
static mesh_admin_cb admin_cb;
static mesh_admin_routing_cb routing_cb;
static meshtastic_AdminMessage pending;
static uint32_t pending_to, pending_id;
mesh_edit_t fake_remote;
int fake_remote_error;
int fake_remote_sent;

void mesh_link_on_admin(mesh_admin_cb on_admin, mesh_admin_routing_cb on_routing) {
    admin_cb = on_admin;
    routing_cb = on_routing;
}

bool mesh_link_send_admin_to(uint32_t to, const struct _meshtastic_AdminMessage *a,
                             bool want_response, uint32_t *id) {
    (void)want_response;
    if (fake_link_state != MESH_LINK_READY)
        return false;
    static uint32_t next_id = 0x9000;
    pending = *a;
    pending_to = to;
    pending_id = ++next_id;
    fake_remote_sent++;
    if (id)
        *id = pending_id;
    return true;
}

bool fake_remote_answer(void) {
    if (!pending_id)
        return false;
    uint32_t id = pending_id;
    pending_id = 0;
    if (fake_remote_error) {
        routing_cb(id, pending_to, (uint8_t)fake_remote_error);
        return true;
    }
    static meshtastic_AdminMessage r;
    memset(&r, 0, sizeof r);
    r.session_passkey.size = 8;
    memset(r.session_passkey.bytes, 0x5a, 8);
    switch (pending.which_payload_variant) {
    case meshtastic_AdminMessage_get_device_metadata_request_tag:
        r.which_payload_variant = meshtastic_AdminMessage_get_device_metadata_response_tag;
        strcpy(r.get_device_metadata_response.firmware_version, "2.6.11.60ec05e");
        break;
    case meshtastic_AdminMessage_get_owner_request_tag:
        r.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
        r.get_owner_response = fake_remote.owner;
        break;
    case meshtastic_AdminMessage_get_config_request_tag: {
        r.which_payload_variant = meshtastic_AdminMessage_get_config_response_tag;
        meshtastic_Config *c = &r.get_config_response;
        c->which_payload_variant = (pb_size_t)(pending.get_config_request + 1);
        switch (c->which_payload_variant) {
        case meshtastic_Config_device_tag: c->payload_variant.device = fake_remote.device; break;
        case meshtastic_Config_position_tag: c->payload_variant.position = fake_remote.position; break;
        case meshtastic_Config_power_tag: c->payload_variant.power = fake_remote.power; break;
        case meshtastic_Config_lora_tag: c->payload_variant.lora = fake_remote.lora; break;
        default: c->payload_variant.bluetooth = fake_remote.bluetooth; break;
        }
        break;
    }
    case meshtastic_AdminMessage_get_channel_request_tag:
        r.which_payload_variant = meshtastic_AdminMessage_get_channel_response_tag;
        r.get_channel_response = fake_remote.channels[pending.get_channel_request - 1];
        r.get_channel_response.index = (int32_t)pending.get_channel_request - 1;
        break;
    default:
        // A change: take it, and ack
        if (pending.which_payload_variant == meshtastic_AdminMessage_set_owner_tag)
            fake_remote.owner = pending.set_owner;
        if (pending.which_payload_variant == meshtastic_AdminMessage_set_config_tag &&
            pending.set_config.which_payload_variant == meshtastic_Config_lora_tag)
            fake_remote.lora = pending.set_config.payload_variant.lora;
        routing_cb(id, pending_to, 0);
        return true;
    }
    static meshtastic_MeshPacket p;
    memset(&p, 0, sizeof p);
    p.from = pending_to;
    p.to = mesh_db.my_node_num;
    p.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum = meshtastic_PortNum_ADMIN_APP;
    p.decoded.request_id = id;
    admin_cb(&p, &r);
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

// Telemetry requests: the last one waits until fake_telemetry_answer()
uint32_t fake_tel_to, fake_tel_id;
pb_size_t fake_tel_variant;
int fake_tel_sent;

bool mesh_link_send_telemetry_req(uint32_t to, pb_size_t variant, uint32_t *id) {
    if (fake_link_state != MESH_LINK_READY)
        return false;
    static uint32_t next_id = 0xa000;
    fake_tel_to = to;
    fake_tel_variant = variant;
    fake_tel_id = ++next_id;
    fake_tel_sent++;
    if (id)
        *id = fake_tel_id;
    return true;
}

// NodeInfo requests: who was asked last, and when (fake_ms)
uint32_t fake_nodeinfo_to;
int fake_nodeinfo_sent;
static uint32_t nodeinfo_ms;

bool mesh_link_send_nodeinfo_req(uint32_t to) {
    if (fake_link_state != MESH_LINK_READY || !to || to == mesh_db.my_node_num)
        return false;
    fake_nodeinfo_to = to;
    nodeinfo_ms = fake_ms;
    fake_nodeinfo_sent++;
    return true;
}

uint32_t mesh_link_nodeinfo_asked(uint32_t node) {
    return node && node == fake_nodeinfo_to ? (nodeinfo_ms ? nodeinfo_ms : 1) : 0;
}
