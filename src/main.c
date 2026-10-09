// calcmesh: Meshtastic client for the PicoCalc.
//
// Bring-up firmware for now: asks the node for its config over uart0 and
// prints a one-line summary of every FromRadio on the USB console.

#include <stdio.h>

#include "pico/rand.h"
#include "pico/stdlib.h"

#include "meshlink.h"

// These are big (hundreds of bytes); keep them off the 2 KiB stack.
static meshtastic_FromRadio rx;
static meshtastic_ToRadio req;

static void print_from_radio(const meshtastic_FromRadio *m) {
    switch (m->which_payload_variant) {
    case meshtastic_FromRadio_my_info_tag:
        printf("my_info: node !%08lx\n", (unsigned long)m->my_info.my_node_num);
        break;
    case meshtastic_FromRadio_node_info_tag:
        printf("node_info: !%08lx %s\n", (unsigned long)m->node_info.num,
               m->node_info.has_user ? m->node_info.user.long_name : "(no user)");
        break;
    case meshtastic_FromRadio_channel_tag:
        printf("channel %ld: %s\n", (long)m->channel.index,
               m->channel.has_settings ? m->channel.settings.name : "");
        break;
    case meshtastic_FromRadio_config_complete_id_tag:
        printf("config complete (id %lu)\n", (unsigned long)m->config_complete_id);
        break;
    case meshtastic_FromRadio_packet_tag: {
        const meshtastic_MeshPacket *p = &m->packet;
        if (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
            p->decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP) {
            printf("text from !%08lx ch%u: %.*s\n", (unsigned long)p->from, p->channel,
                   (int)p->decoded.payload.size, (const char *)p->decoded.payload.bytes);
        } else {
            printf("packet from !%08lx\n", (unsigned long)p->from);
        }
        break;
    }
    default:
        printf("from_radio variant %u\n", m->which_payload_variant);
        break;
    }
}

static void node_log(char c) {
    putchar(c);
}

int main(void) {
    stdio_init_all();
    meshlink_init();
    meshlink_set_log_cb(node_log);

    sleep_ms(2000); // time to open the USB console
    printf("calcmesh up\n");

    meshlink_wake();
    req.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
    req.want_config_id = get_rand_32();
    meshlink_send(&req);

    for (;;) {
        if (meshlink_poll(&rx))
            print_from_radio(&rx);
        tight_loop_contents();
    }
}
