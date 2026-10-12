// Chats tab: a list of conversations (enabled channels, then DMs, newest
// first), and the conversation view with its compose line.

#include <stdio.h>
#include <string.h>

#include "link.h"
#include "ui.h"
#include "ui_internal.h"

#define LIST_ROWS (UI_BODY_H / UI_LH)

// Message layout: 4-cell sender, delivery mark, then the text
#define PREFIX 6
#define TEXT_W (UI_COLS - PREFIX - 1)
#define DRAFT_W (UI_COLS - 2)

static ui_list_t list;
static bool in_conv;
static mesh_conv_t conv;
static int scroll; // lines scrolled up from the newest

static char draft[MESH_MSG_SEND_MAX + 1];
static int cursor; // byte offset into draft
static const char *notice;
static char notice_buf[48];

// --- conversation list -------------------------------------------------------

static int channel_count(int *idx) {
    int n = 0;
    for (int i = 0; i < MESH_MAX_CHANNELS; i++)
        if (mesh_db.channels[i].role != 0) // 0 = DISABLED
            idx[n++] = i;
    return n;
}

static int dm_count(void) {
    int n = 0, convs = mesh_msgs_conv_count();
    for (int i = 0; i < convs; i++)
        n += MESH_CONV_IS_DM(mesh_msgs_conv_at(i)->conv);
    return n;
}

static int conv_count(void) {
    int idx[MESH_MAX_CHANNELS];
    return channel_count(idx) + dm_count();
}

// Channels in index order, then DMs, most recently active first
static mesh_conv_t conv_at(int i) {
    int idx[MESH_MAX_CHANNELS];
    int nch = channel_count(idx);
    if (i < nch)
        return MESH_CONV_CHANNEL(idx[i]);
    i -= nch;
    int convs = mesh_msgs_conv_count();
    for (int j = 0; j < convs; j++) {
        mesh_conv_t c = mesh_msgs_conv_at(j)->conv;
        if (MESH_CONV_IS_DM(c) && i-- == 0)
            return c;
    }
    return MESH_CONV_CHANNEL(0);
}

static void conv_title(mesh_conv_t c, char *buf, size_t len) {
    if (MESH_CONV_IS_DM(c))
        snprintf(buf, len, "@%s", ui_node_long(c.peer));
    else
        snprintf(buf, len, "#%s", mesh_db_channel_name(c.channel));
}

static int unread(mesh_conv_t c) {
    const mesh_conv_info_t *ci = mesh_msgs_conv(c);
    return ci ? ci->unread : 0;
}

int ui_chats_unread(void) { return (int)mesh_msgs_unread_total(); }

static void draw_list(void) {
    int n = conv_count();
    ui_list_fix(&list, n, LIST_ROWS);
    int y = UI_BODY_Y + 1;
    if (!n) {
        ui_cells(1, y + UI_LH, UI_COLS - 2, "No channels yet.", FB_GREY, FB_CLEAR);
        return;
    }
    for (int i = list.top; i < n && i < list.top + LIST_ROWS; i++, y += UI_LH) {
        mesh_conv_t c = conv_at(i);
        bool sel = i == list.sel;
        uint8_t fg = sel ? FB_BLACK : FB_WHITE;
        uint8_t dim = sel ? FB_BLACK : FB_GREY;
        char title[48], buf[MESH_MSG_TEXT_MAX + 16];

        if (sel)
            fb_fill(0, y, FB_W, UI_LH, FB_WHITE);
        conv_title(c, title, sizeof title);
        ui_cells(1, y, 17, title, fg, FB_CLEAR);

        int u = unread(c);
        if (u) {
            // Count as an inverted badge
            snprintf(buf, sizeof buf, "%d", u > 999 ? 999 : u);
            int w = utf8_cells(buf);
            fb_fill((22 - w) * UI_CW - 2, y, w * UI_CW + 3, UI_LH - 1,
                    sel ? FB_BLACK : FB_WHITE);
            ui_cells_right(18, y, 4, buf, sel ? FB_WHITE : FB_BLACK, FB_CLEAR);
        }

        const mesh_msg_t *m = mesh_msgs_recent(c, 0);
        if (m) {
            snprintf(buf, sizeof buf, "%s: %s", ui_node_short(m->from), m->text);
            for (char *p = buf; *p; p++)
                if (*p == '\n')
                    *p = ' ';
            ui_cells(24, y, UI_COLS - 25, buf, dim, FB_CLEAR);
        } else {
            ui_cells(24, y, UI_COLS - 25, "-", sel ? FB_BLACK : FB_DIM, FB_CLEAR);
        }
    }
}

// --- conversation ------------------------------------------------------------

// Length in bytes of the next line of s wrapped to width cells, breaking
// after a space where possible. *next is where the following line starts.
static int wrap(const char *s, int width, const char **next) {
    const char *p = s, *brk = NULL;
    int cells = 0;
    uint32_t cp;
    while (*p && *p != '\n') {
        if (cells == width) {
            if (brk) {
                *next = brk;
                return (int)(brk - s);
            }
            *next = p;
            return (int)(p - s);
        }
        const char *q = utf8_next(p, &cp);
        if (cp == ' ')
            brk = q;
        p = q;
        cells++;
    }
    *next = *p ? p + 1 : p;
    return (int)(p - s);
}

static int line_count(const char *s) {
    int n = 0;
    const char *next;
    do {
        wrap(s, TEXT_W, &next);
        s = next;
        n++;
    } while (*s);
    return n;
}

// Delivery mark: a hollow square that brightens as the message gets
// further, filled once the recipient acks, ! if it failed
static void mark(int x, int y, uint8_t status) {
    int bx = x + 3, by = y + 4;
    switch (status) {
    case MESH_MSG_QUEUED: fb_rect(bx, by, 4, 4, FB_DIM); break;
    case MESH_MSG_SENT: fb_rect(bx, by, 4, 4, FB_GREY); break;
    case MESH_MSG_RELAYED: fb_rect(bx, by, 4, 4, FB_WHITE); break;
    case MESH_MSG_DELIVERED: fb_fill(bx, by, 4, 4, FB_WHITE); break;
    case MESH_MSG_FAILED: fb_text(x + 3, y, "!", 1, UI_FONT, FB_WHITE, FB_CLEAR); break;
    default: break;
    }
}

static void draw_message(const mesh_msg_t *m, int y) {
    bool mine = m->from == mesh_db.my_node_num || m->status != MESH_MSG_RECEIVED;
    ui_cells(0, y, 4, ui_node_short(m->from), mine ? FB_GREY : FB_WHITE, FB_CLEAR);
    if (mine)
        mark(4 * UI_CW, y, m->status);
    const char *s = m->text, *next;
    do {
        int len = wrap(s, TEXT_W, &next);
        char line[MESH_MSG_TEXT_MAX + 1];
        memcpy(line, s, (size_t)len);
        line[len] = '\0';
        ui_cells(PREFIX, y, TEXT_W, line, FB_WHITE, FB_CLEAR);
        y += UI_LH;
        s = next;
    } while (*s);
}

static int draft_lines(void) {
    int n = (int)strlen(draft) + 1; // room for the cursor after the end
    return (n + DRAFT_W - 1) / DRAFT_W;
}

static void draw_compose(int y) {
    char buf[24];
    fb_hline(0, y, FB_W, FB_DIM);
    if (notice) {
        int w = utf8_cells(notice);
        fb_fill(2, y - UI_LH / 2, (w + 2) * UI_CW, UI_LH, FB_BLACK);
        ui_cells(1, y - UI_LH / 2, w + 1, notice, FB_WHITE, FB_CLEAR);
    }
    if (*draft) {
        snprintf(buf, sizeof buf, " %u/%u ", (unsigned)strlen(draft),
                 MESH_MSG_SEND_MAX);
        int w = utf8_cells(buf);
        fb_fill((UI_COLS - w - 1) * UI_CW, y - UI_LH / 2, w * UI_CW, UI_LH, FB_BLACK);
        ui_cells(UI_COLS - w - 1, y - UI_LH / 2, w, buf, FB_DIM, FB_CLEAR);
    }
    y += 2;
    ui_cells(0, y, 1, ">", FB_GREY, FB_CLEAR);
    int len = (int)strlen(draft);
    for (int i = 0; i <= len; i += DRAFT_W) {
        char line[DRAFT_W + 1];
        int n = len - i < DRAFT_W ? len - i : DRAFT_W;
        memcpy(line, draft + i, (size_t)n);
        line[n] = '\0';
        ui_cells(2, y + (i / DRAFT_W) * UI_LH, DRAFT_W, line, FB_WHITE, FB_CLEAR);
    }
    // Block cursor, the character under it inverted
    int cx = 2 + cursor % DRAFT_W, cy = y + (cursor / DRAFT_W) * UI_LH;
    char under[2] = {draft[cursor] ? draft[cursor] : ' ', 0};
    ui_cells(cx, cy, 1, under, FB_BLACK, FB_WHITE);
}

// A DM whose latest message failed for want of a key: say what's going on,
// since "failed" alone looks like a radio problem (link.h).
static const char *key_hint(void) {
    if (!conv.peer)
        return NULL;
    for (int i = 0; i < mesh_msgs_count(conv); i++) {
        const mesh_msg_t *m = mesh_msgs_recent(conv, i);
        if (m->status == MESH_MSG_RECEIVED)
            continue;
        if (m->status != MESH_MSG_FAILED)
            return NULL;
        const char *who = ui_node_short(conv.peer);
        switch (m->error) {
        case meshtastic_Routing_Error_PKI_SEND_FAIL_PUBLIC_KEY:
        case meshtastic_Routing_Error_PKI_FAILED:
            snprintf(notice_buf, sizeof notice_buf, "No key for %s yet; %s", who,
                     mesh_link_nodeinfo_asked(conv.peer) ? "asked it for NodeInfo"
                                                        : "needs its NodeInfo");
            return notice_buf;
        case meshtastic_Routing_Error_PKI_UNKNOWN_PUBKEY:
            snprintf(notice_buf, sizeof notice_buf, "%s lacks our key; sent it ours", who);
            return notice_buf;
        default:
            return NULL;
        }
    }
    return NULL;
}

static void draw_conv(void) {
    char title[64];
    int y = UI_BODY_Y + 1;
    conv_title(conv, title, sizeof title);
    ui_cells(0, y, UI_COLS - 10, title, FB_WHITE, FB_CLEAR);
    ui_cells_right(UI_COLS - 10, y, 10, "Esc: back", FB_DIM, FB_CLEAR);
    y += UI_LH + 1;
    fb_hline(0, y - 1, FB_W, FB_DIM);

    int compose_h = draft_lines() * UI_LH + 3;
    int bottom = UI_BODY_Y + UI_BODY_H - compose_h; // compose rule
    // A key hint stays up, so the messages make room for it rather than
    // being written over
    const char *hint = notice ? NULL : key_hint();
    int msgs_end = hint ? bottom - UI_LH / 2 - 1 : bottom;
    int area = msgs_end - y - 1;
    int rows = area / UI_LH;

    int n = mesh_msgs_count(conv);
    int total = 0;
    for (int i = 0; i < n; i++)
        total += line_count(mesh_msgs_recent(conv, i)->text);
    if (scroll > total - rows)
        scroll = total - rows;
    if (scroll < 0)
        scroll = 0;

    // Newest at the bottom; lay out upwards, clipped to the message area
    fb_clip(0, y, FB_W, area);
    int line_y = msgs_end - 1 + scroll * UI_LH;
    for (int i = 0; i < n && line_y > y; i++) {
        const mesh_msg_t *m = mesh_msgs_recent(conv, i);
        line_y -= line_count(m->text) * UI_LH;
        if (line_y < msgs_end)
            draw_message(m, line_y);
    }
    if (!n)
        ui_cells(1, y + UI_LH, UI_COLS - 2, "No messages yet.", FB_GREY, FB_CLEAR);
    if (total > rows) {
        int h = rows * UI_LH;
        int bh = h * rows / total;
        int by = y + h - bh - h * scroll / total;
        fb_vline(FB_W - 2, y, h, FB_DIM);
        fb_fill(FB_W - 3, by, 3, bh < 4 ? 4 : bh, FB_WHITE);
    }
    fb_clip(0, UI_BODY_Y, FB_W, UI_BODY_H);

    // Only while nothing else is being said; it isn't kept
    if (hint)
        notice = hint;
    draw_compose(bottom);
    if (hint)
        notice = NULL;
    if (unread(conv))
        mesh_msgs_mark_read(conv);
}

void ui_chats_draw(void) {
    if (in_conv)
        draw_conv();
    else
        draw_list();
}

void ui_chats_open(mesh_conv_t c) {
    if (!in_conv || c.peer != conv.peer || c.channel != conv.channel) {
        draft[0] = '\0';
        cursor = 0;
    }
    conv = c;
    in_conv = true;
    scroll = 0;
    notice = NULL;
    ui_switch_tab(UI_TAB_CHATS);
}

static bool conv_key(const kbd_event_t *ev) {
    int len = (int)strlen(draft);
    int page = (UI_BODY_H - 3 * UI_LH) / UI_LH;
    notice = NULL;

    switch (ev->key) {
    case KBD_ESC: in_conv = false; return true;
    case KBD_UP: scroll++; return true;
    case KBD_DOWN: scroll--; return true;
    case KBD_PGUP: scroll += page; return true;
    case KBD_PGDN: scroll -= page; return true;
    case KBD_LEFT: if (cursor > 0) cursor--; return true;
    case KBD_RIGHT: if (cursor < len) cursor++; return true;
    case KBD_HOME: cursor = 0; return true;
    case KBD_END: cursor = len; return true;
    case KBD_BACKSPACE:
        if (cursor > 0) {
            memmove(draft + cursor - 1, draft + cursor, (size_t)(len - cursor + 1));
            cursor--;
        }
        return true;
    case KBD_DEL:
        if (cursor < len)
            memmove(draft + cursor, draft + cursor + 1, (size_t)(len - cursor));
        return true;
    case KBD_ENTER:
        if (!len)
            return true;
        mesh_send_result_t r = mesh_link_send_text(conv, draft, (size_t)len, 0, NULL);
        if (r == MESH_SEND_OK) {
            draft[0] = '\0';
            cursor = 0;
            scroll = 0;
        } else {
            snprintf(notice_buf, sizeof notice_buf, "Not sent: %s",
                     mesh_link_send_result_name(r));
            notice = notice_buf;
        }
        return true;
    default:
        break;
    }
    if (ev->key >= 0x20 && ev->key < 0x7F && !(ev->mods & (KBD_MOD_CTRL | KBD_MOD_ALT))) {
        if (len >= MESH_MSG_SEND_MAX) {
            notice = "Message full";
            return true;
        }
        memmove(draft + cursor + 1, draft + cursor, (size_t)(len - cursor + 1));
        draft[cursor++] = (char)ev->key;
        return true;
    }
    return false;
}

bool ui_chats_key(const kbd_event_t *ev) {
    if (in_conv)
        return conv_key(ev);
    int n = conv_count();
    if (ui_list_key(&list, n, LIST_ROWS, ev->key))
        return true;
    if (ev->key == KBD_ENTER && n) {
        ui_chats_open(conv_at(list.sel));
        return true;
    }
    return false;
}
