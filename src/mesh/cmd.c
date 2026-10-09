#include "cmd.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "link.h"
#include "msgs.h"
#include "nodedb.h"

#define MSGS_DEFAULT 10

// Split off the next space-delimited word; *rest is left at what follows,
// with leading blanks skipped.
static char *word(char **rest) {
    char *s = *rest;
    while (*s == ' ' || *s == '\t')
        s++;
    if (!*s) {
        *rest = s;
        return NULL;
    }
    char *w = s;
    while (*s && *s != ' ' && *s != '\t')
        s++;
    if (*s)
        *s++ = '\0';
    while (*s == ' ' || *s == '\t')
        s++;
    *rest = s;
    return w;
}

// "!a1b2c3d4", "0xa1b2c3d4", a decimal node number, or a short name.
static uint32_t parse_node(const char *s) {
    char *end;
    unsigned long v;
    if (s[0] == '!') {
        v = strtoul(s + 1, &end, 16);
        return *end || end == s + 1 ? 0 : (uint32_t)v;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        v = strtoul(s + 2, &end, 16);
        return *end || end == s + 2 ? 0 : (uint32_t)v;
    }
    if (isdigit((unsigned char)s[0])) {
        v = strtoul(s, &end, 10);
        return *end ? 0 : (uint32_t)v;
    }
    for (int i = 0; i < mesh_db.node_count; i++) {
        const mesh_node_t *n = &mesh_db.nodes[i];
        if (n->has_user && strcasecmp(n->short_name, s) == 0)
            return n->num;
    }
    return 0;
}

static bool parse_channel(const char *s, uint8_t *ch) {
    if (s[0] < '0' || s[0] >= '0' + MESH_MAX_CHANNELS || s[1])
        return false;
    *ch = (uint8_t)(s[0] - '0');
    return true;
}

static void send(mesh_conv_t conv, char *text) {
    size_t n = strlen(text);
    while (n && (text[n - 1] == ' ' || text[n - 1] == '\t'))
        text[--n] = '\0';
    mesh_msg_t *m;
    mesh_send_result_t r = mesh_link_send_text(conv, text, strlen(text), 0, &m);
    if (r == MESH_SEND_OK)
        mesh_msgs_print(m);
    else if (r == MESH_SEND_TOO_LONG)
        printf("too long: %u bytes, the limit is %u\n", (unsigned)strlen(text),
               MESH_MSG_SEND_MAX);
    else
        printf("not sent: %s\n", mesh_link_send_result_name(r));
}

static void conv_label(mesh_conv_t c, char *buf, size_t len) {
    if (!c.peer) {
        snprintf(buf, len, "ch%u %s", c.channel, mesh_db_channel_name(c.channel));
        return;
    }
    const mesh_node_t *n = mesh_db_find(c.peer);
    if (n && n->has_user)
        snprintf(buf, len, "dm %s (%s)", n->short_name, n->long_name);
    else
        snprintf(buf, len, "dm !%08lx", (unsigned long)c.peer);
}

static void list_convs(void) {
    int n = mesh_msgs_conv_count();
    if (!n) {
        printf("no messages yet\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        const mesh_conv_info_t *c = mesh_msgs_conv_at(i);
        char label[64];
        conv_label(c->conv, label, sizeof label);
        printf("  %-36s %3u msgs", label, c->count);
        if (c->unread)
            printf(", %u unread", c->unread);
        printf("\n");
    }
}

static void show_conv(mesh_conv_t conv, int count) {
    int n = mesh_msgs_count(conv);
    if (count > n)
        count = n;
    if (!count)
        printf("no messages\n");
    for (int i = count - 1; i >= 0; i--)
        mesh_msgs_print(mesh_msgs_recent(conv, i));
    mesh_msgs_mark_read(conv);
}

bool mesh_cmd_run(char *line) {
    char *rest = line;
    char *cmd = word(&rest);
    if (!cmd)
        return true;

    if (strcmp(cmd, "send") == 0) {
        char *ch = word(&rest);
        uint8_t c;
        if (!ch || !parse_channel(ch, &c) || !*rest) {
            printf("usage: send <channel 0-7> <text>\n");
            return true;
        }
        send(MESH_CONV_CHANNEL(c), rest);
    } else if (strcmp(cmd, "dm") == 0) {
        char *who = word(&rest);
        uint32_t node = who ? parse_node(who) : 0;
        if (!who || !*rest) {
            printf("usage: dm <!nodeid|shortname> <text>\n");
            return true;
        }
        if (!node || node == MESH_ADDR_BROADCAST) {
            printf("unknown node '%s'\n", who);
            return true;
        }
        send(MESH_CONV_DM(node), rest);
    } else if (strcmp(cmd, "msgs") == 0) {
        char *which = word(&rest);
        char *count = word(&rest);
        if (!which) {
            list_convs();
            return true;
        }
        uint8_t c;
        mesh_conv_t conv;
        if (parse_channel(which, &c)) {
            conv = MESH_CONV_CHANNEL(c);
        } else {
            uint32_t node = parse_node(which);
            if (!node) {
                printf("unknown channel or node '%s'\n", which);
                return true;
            }
            conv = MESH_CONV_DM(node);
        }
        show_conv(conv, count ? atoi(count) : MSGS_DEFAULT);
    } else if (strcmp(cmd, "nodes") == 0) {
        mesh_db_dump();
    } else {
        return false;
    }
    return true;
}

void mesh_cmd_help(void) {
    printf("send <ch> <text>       send to channel 0-7\n"
           "dm <node> <text>       direct message; node is !id or short name\n"
           "msgs [<ch|node> [n]]   list conversations, or show the last n\n"
           "                       messages of one (and mark it read)\n"
           "nodes                  node and channel tables\n");
}
