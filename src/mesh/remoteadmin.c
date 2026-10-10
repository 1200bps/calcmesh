#include "remoteadmin.h"

#include <stdio.h>
#include <string.h>

#include "link.h"

#include "meshtastic/admin.pb.h"

ra_remote_t ra;

typedef enum {
    S_META,
    S_OWNER,
    S_CONFIG,  // arg: Config payload tag
    S_CHANNEL, // arg: index
    S_BEGIN,
    S_SET_OWNER,
    S_SET_CONFIG,
    S_SET_CHANNEL,
    S_COMMIT,
    S_WAIT, // RA_RESTART_MS
} step_kind_t;

typedef struct {
    uint8_t kind, arg;
} step_t;

#define MAX_STEPS 32
static step_t plan[MAX_STEPS];
static int nsteps, cur;
static ra_state_t state;
static uint8_t error;
static bool writing, committed;
static uint8_t abort_error; // a write failed partway: commit, then fail with this

static uint32_t step_id, step_ms;
static mesh_edit_t out; // what a write sends

static uint8_t passkey[8];
static uint8_t passkey_len;

static meshtastic_AdminMessage msg; // static: ~500 bytes

static const struct {
    uint32_t bit;
    pb_size_t tag;
} sections[] = {
    {MESH_EDIT_DEVICE, meshtastic_Config_device_tag},
    {MESH_EDIT_POSITION, meshtastic_Config_position_tag},
    {MESH_EDIT_POWER, meshtastic_Config_power_tag},
    {MESH_EDIT_LORA, meshtastic_Config_lora_tag},
    {MESH_EDIT_BLUETOOTH, meshtastic_Config_bluetooth_tag},
};
#define N_SECTIONS (sizeof sections / sizeof sections[0])

static uint32_t bit_of_tag(pb_size_t tag) {
    for (size_t i = 0; i < N_SECTIONS; i++)
        if (sections[i].tag == tag)
            return sections[i].bit;
    return 0;
}

static void on_admin(const meshtastic_MeshPacket *p, const meshtastic_AdminMessage *a);
static void on_routing(uint32_t request_id, uint32_t from, uint8_t err);

void ra_select(uint32_t node) {
    mesh_link_on_admin(on_admin, on_routing);
    if (node == ra.node)
        return;
    if (state == RA_BUSY)
        state = RA_IDLE; // whatever it was waiting for no longer matters
    uint32_t v = ra.version;
    memset(&ra, 0, sizeof ra);
    ra.node = node;
    ra.version = v + 1;
    passkey_len = 0;
    state = RA_IDLE;
    nsteps = cur = 0;
    writing = committed = false;
}

static void add(step_kind_t k, unsigned arg) {
    if (nsteps < MAX_STEPS)
        plan[nsteps++] = (step_t){(uint8_t)k, (uint8_t)arg};
}

static void fail(uint8_t err) {
    if (writing && committed == false && cur > 0 && plan[cur].kind != S_COMMIT && abort_error == 0) {
        // Partway through a write: close the transaction, which an
        // uncommitted one would hold open on the node, then fail
        abort_error = err;
        for (int i = cur; i < nsteps; i++)
            if (plan[i].kind == S_COMMIT) {
                cur = i;
                step_id = 0;
                return;
            }
    }
    error = abort_error ? abort_error : err;
    state = RA_FAILED;
    printf("[remote] !%08lx: failed at step %d/%d (error %u)\n", (unsigned long)ra.node, cur + 1,
           nsteps, error);
}

static void next(void) {
    if (abort_error && plan[cur].kind == S_COMMIT)
        cur = nsteps - 1; // the write had failed: that commit was the last of it
    cur++;
    step_id = 0;
    if (cur >= nsteps) {
        if (abort_error) {
            error = abort_error;
            state = RA_FAILED;
        } else {
            state = RA_DONE;
        }
    }
}

static bool start(void) {
    const step_t *s = &plan[cur];
    memset(&msg, 0, sizeof msg);
    bool get = true;
    switch (s->kind) {
    case S_META:
        msg.which_payload_variant = meshtastic_AdminMessage_get_device_metadata_request_tag;
        msg.get_device_metadata_request = true;
        break;
    case S_OWNER:
        msg.which_payload_variant = meshtastic_AdminMessage_get_owner_request_tag;
        msg.get_owner_request = true;
        break;
    case S_CONFIG:
        msg.which_payload_variant = meshtastic_AdminMessage_get_config_request_tag;
        // ConfigType counts from 0 where the payload tags count from 1
        msg.get_config_request = (meshtastic_AdminMessage_ConfigType)(s->arg - 1);
        break;
    case S_CHANNEL:
        msg.which_payload_variant = meshtastic_AdminMessage_get_channel_request_tag;
        msg.get_channel_request = s->arg + 1u; // index + 1, so it's never 0
        break;
    case S_WAIT:
        step_id = 1;
        step_ms = mesh_link_ms();
        return true;
    default:
        get = false;
        break;
    }
    if (!get) {
        switch (s->kind) {
        case S_BEGIN:
            msg.which_payload_variant = meshtastic_AdminMessage_begin_edit_settings_tag;
            msg.begin_edit_settings = true;
            break;
        case S_COMMIT:
            msg.which_payload_variant = meshtastic_AdminMessage_commit_edit_settings_tag;
            msg.commit_edit_settings = true;
            break;
        case S_SET_OWNER:
            msg.which_payload_variant = meshtastic_AdminMessage_set_owner_tag;
            msg.set_owner = out.owner;
            break;
        case S_SET_CHANNEL:
            msg.which_payload_variant = meshtastic_AdminMessage_set_channel_tag;
            msg.set_channel = out.channels[s->arg];
            break;
        case S_SET_CONFIG: {
            msg.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
            meshtastic_Config *c = &msg.set_config;
            c->which_payload_variant = s->arg;
            switch (s->arg) {
            case meshtastic_Config_device_tag: c->payload_variant.device = out.device; break;
            case meshtastic_Config_position_tag: c->payload_variant.position = out.position; break;
            case meshtastic_Config_power_tag: c->payload_variant.power = out.power; break;
            case meshtastic_Config_lora_tag: c->payload_variant.lora = out.lora; break;
            default: c->payload_variant.bluetooth = out.bluetooth; break;
            }
            break;
        }
        default: break;
        }
        // Anything that changes something needs the session key
        memcpy(msg.session_passkey.bytes, passkey, passkey_len);
        msg.session_passkey.size = passkey_len;
    }
    step_ms = mesh_link_ms();
    if (!mesh_link_send_admin_to(ra.node, &msg, get, &step_id)) {
        step_id = 0;
        return false;
    }
    return true;
}

static bool begin(void) {
    if (!ra.node || mesh_link_state() != MESH_LINK_READY) {
        error = RA_ERR_NO_LINK;
        return false;
    }
    mesh_link_on_admin(on_admin, on_routing);
    cur = 0;
    error = abort_error = 0;
    committed = false;
    state = RA_BUSY;
    if (!start()) {
        fail(RA_ERR_NO_LINK);
        return false;
    }
    return true;
}

static void add_gets(uint32_t s) {
    if (s & RA_META)
        add(S_META, 0);
    if (s & MESH_EDIT_OWNER)
        add(S_OWNER, 0);
    for (size_t i = 0; i < N_SECTIONS; i++)
        if (s & sections[i].bit)
            add(S_CONFIG, sections[i].tag);
    for (int i = 0; i < MESH_MAX_CHANNELS; i++)
        if (s & MESH_EDIT_CHANNEL(i))
            add(S_CHANNEL, (unsigned)i);
}

bool ra_fetch(uint32_t s) {
    if (state == RA_BUSY || !s)
        return false;
    nsteps = 0;
    writing = false;
    add_gets(s);
    return begin();
}

bool ra_write(const mesh_edit_t *e, uint32_t s) {
    if (state == RA_BUSY || !s)
        return false;
    out = *e;
    nsteps = 0;
    writing = true;
    add(S_OWNER, 0); // for a fresh session key
    add(S_BEGIN, 0);
    if (s & MESH_EDIT_OWNER)
        add(S_SET_OWNER, 0);
    for (size_t i = 0; i < N_SECTIONS; i++)
        if (s & sections[i].bit)
            add(S_SET_CONFIG, sections[i].tag);
    for (int i = 0; i < MESH_MAX_CHANNELS; i++)
        if (s & MESH_EDIT_CHANNEL(i))
            add(S_SET_CHANNEL, (unsigned)i);
    add(S_COMMIT, 0);
    add(S_WAIT, 0);
    // Read back what was written. Not the first S_OWNER's answer: the node
    // was still on the old settings then.
    add_gets(s);
    return begin();
}

// --- answers -------------------------------------------------------------------

static void store_channel(const meshtastic_Channel *c) {
    if (c->index < 0 || c->index >= MESH_MAX_CHANNELS)
        return;
    ra.s.channels[c->index] = *c;
    ra.have |= MESH_EDIT_CHANNEL(c->index);
    if (c->role != meshtastic_Channel_Role_DISABLED || c->index == 0)
        return;
    // Channels after a disabled one: assume disabled too, and don't ask
    for (int i = cur + 1; i < nsteps; i++)
        if (plan[i].kind == S_CHANNEL && plan[i].arg > c->index) {
            int n = plan[i].arg;
            memset(&ra.s.channels[n], 0, sizeof ra.s.channels[n]);
            ra.s.channels[n].index = n;
            ra.have |= MESH_EDIT_CHANNEL(n);
            memmove(&plan[i], &plan[i + 1], (size_t)(nsteps - i - 1) * sizeof plan[0]);
            nsteps--;
            i--;
        }
}

static void on_admin(const meshtastic_MeshPacket *p, const meshtastic_AdminMessage *a) {
    if (!ra.node || p->from != ra.node)
        return;
    if (a->session_passkey.size == sizeof passkey) {
        memcpy(passkey, a->session_passkey.bytes, sizeof passkey);
        passkey_len = sizeof passkey;
    }
    if (state != RA_BUSY || !step_id || p->decoded.request_id != step_id)
        return;
    const step_t *s = &plan[cur];
    bool ok = false;
    switch (a->which_payload_variant) {
    case meshtastic_AdminMessage_get_device_metadata_response_tag:
        if (s->kind == S_META) {
            snprintf(ra.firmware, sizeof ra.firmware, "%s",
                     a->get_device_metadata_response.firmware_version);
            ra.have |= RA_META;
            ok = true;
        }
        break;
    case meshtastic_AdminMessage_get_owner_response_tag:
        if (s->kind == S_OWNER) {
            // The write's first S_OWNER is only for the key; its answer is
            // still good to show if the write fails
            ra.s.owner = a->get_owner_response;
            ra.have |= MESH_EDIT_OWNER;
            ok = true;
        }
        break;
    case meshtastic_AdminMessage_get_config_response_tag: {
        const meshtastic_Config *c = &a->get_config_response;
        if (s->kind != S_CONFIG || c->which_payload_variant != s->arg)
            break;
        switch (c->which_payload_variant) {
        case meshtastic_Config_device_tag: ra.s.device = c->payload_variant.device; break;
        case meshtastic_Config_position_tag: ra.s.position = c->payload_variant.position; break;
        case meshtastic_Config_power_tag: ra.s.power = c->payload_variant.power; break;
        case meshtastic_Config_lora_tag: ra.s.lora = c->payload_variant.lora; break;
        default: ra.s.bluetooth = c->payload_variant.bluetooth; break;
        }
        ra.have |= bit_of_tag(c->which_payload_variant);
        ok = true;
        break;
    }
    case meshtastic_AdminMessage_get_channel_response_tag:
        if (s->kind == S_CHANNEL && a->get_channel_response.index == s->arg) {
            store_channel(&a->get_channel_response);
            ok = true;
        }
        break;
    default: break;
    }
    if (ok) {
        ra.version++;
        next();
        if (state == RA_BUSY && !start())
            fail(RA_ERR_NO_LINK);
    }
}

static bool is_set(step_kind_t k) {
    return k == S_BEGIN || k == S_SET_OWNER || k == S_SET_CONFIG || k == S_SET_CHANNEL ||
           k == S_COMMIT;
}

static void on_routing(uint32_t request_id, uint32_t from, uint8_t err) {
    if (state != RA_BUSY || !step_id || request_id != step_id)
        return;
    const step_t *s = &plan[cur];
    if (err != meshtastic_Routing_Error_NONE) {
        if (s->kind == S_COMMIT) {
            // The commit got there or it didn't; read back to find out
            committed = true;
            next();
        } else {
            fail(err);
            if (state == RA_FAILED)
                return;
        }
    } else if (from == ra.node && is_set((step_kind_t)s->kind)) {
        // Acked by the node itself (not just relayed on by a neighbour)
        if (s->kind == S_COMMIT)
            committed = true;
        next();
    } else {
        return;
    }
    if (state == RA_BUSY && !start())
        fail(RA_ERR_NO_LINK);
}

void ra_poll(void) {
    if (state != RA_BUSY)
        return;
    uint32_t t = mesh_link_ms() - step_ms;
    const step_t *s = &plan[cur];
    if (s->kind == S_WAIT) {
        if (t < RA_RESTART_MS)
            return;
        next();
    } else if (!step_id) {
        // Not sent yet (the link was busy): try again below
    } else if (t < RA_STEP_MS) {
        return;
    } else if (s->kind == S_COMMIT) {
        // No ack, perhaps lost to the restart; the read-back will tell
        committed = true;
        next();
    } else {
        fail(RA_ERR_NO_ANSWER);
        if (state != RA_BUSY)
            return;
        // Failing partway through a write moved us on to the commit
    }
    if (state == RA_BUSY && !start())
        fail(RA_ERR_NO_LINK);
}

ra_state_t ra_state(void) { return state; }
int ra_done(void) { return cur < nsteps ? cur : nsteps; }
int ra_total(void) { return nsteps; }
uint8_t ra_error(void) { return error; }
bool ra_writing(void) { return writing; }
bool ra_written(void) { return writing && committed; }
