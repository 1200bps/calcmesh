#include "sd/msglog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ff.h"
#include "meshtastic/mesh.pb.h"
#include "mesh/nodedb.h"

#define HEADER "n\ttime\tfrom\tto\tch\tid\treply\tstatus\terr\tsnr\trssi\thops\tflags\ttext\n"
#define STATUS_W 13 // "delivered" padded to 9, a tab, 3 digits
#define QUEUE_BYTES 4096
#define PENDING 16 // outgoing messages whose status we may still rewrite
#define FILES (MESH_MAX_CHANNELS + MESH_MAX_DMS)
#define PATCHES 8 // in-flight messages failed per file at restore

static const char *const status_words[] = {
    [MESH_MSG_RECEIVED] = "received", [MESH_MSG_QUEUED] = "sending",
    [MESH_MSG_SENT] = "sent",         [MESH_MSG_RELAYED] = "relayed",
    [MESH_MSG_DELIVERED] = "delivered", [MESH_MSG_FAILED] = "failed",
};

static bool online;
static uint32_t next_n = 1;
static msglog_stats_t stats;
static FIL fil;
static char line[MSGLOG_LINE_MAX + 1];

// ---- time -------------------------------------------------------------------

// Days since 1970-01-01 <-> civil date (H. Hinnant's algorithms).
static int32_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

static void civil_from_days(int32_t z, int *y, unsigned *m, unsigned *d) {
    z += 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)yoe + era * 400 + (*m <= 2);
}

uint32_t msglog_fattime(uint32_t epoch) {
    int y;
    unsigned mo, d;
    civil_from_days((int32_t)(epoch / 86400), &y, &mo, &d);
    if (!epoch || y < 1980 || y > 2107)
        return (uint32_t)(2026 - 1980) << 25 | 1u << 21 | 1u << 16;
    uint32_t s = epoch % 86400;
    return (uint32_t)(y - 1980) << 25 | mo << 21 | d << 16 | (s / 3600) << 11 |
           (s / 60 % 60) << 5 | (s % 60) / 2;
}

// ---- records ----------------------------------------------------------------

static size_t put_status(char *p, uint8_t status, uint8_t error) {
    const char *w = status < sizeof status_words / sizeof *status_words
                        ? status_words[status]
                        : NULL;
    return (size_t)sprintf(p, "%-9s\t%03u", w ? w : "?", error);
}

size_t msglog_format(const mesh_msg_t *m, uint32_t n, char *buf, size_t *status_off) {
    char *p = buf;
    p += sprintf(p, "%lu\t", (unsigned long)n);
    if (m->rx_time) {
        int y;
        unsigned mo, d, s = m->rx_time % 86400;
        civil_from_days((int32_t)(m->rx_time / 86400), &y, &mo, &d);
        p += sprintf(p, "%04d-%02u-%02uT%02u:%02u:%02uZ\t", y, mo, d, s / 3600,
                     s / 60 % 60, s % 60);
    } else {
        p += sprintf(p, "-\t");
    }
    p += sprintf(p, "!%08lx\t!%08lx\t%u\t%08lx\t%08lx\t", (unsigned long)m->from,
                 (unsigned long)m->to, m->channel, (unsigned long)m->id,
                 (unsigned long)m->reply_id);
    *status_off = (size_t)(p - buf);
    p += put_status(p, m->status, m->status == MESH_MSG_FAILED ? m->error : 0);
    if (m->status == MESH_MSG_RECEIVED) {
        p += sprintf(p, "\t%.2f\t%d\t", (double)m->snr, m->rssi);
        p += m->hops == 0xFF ? sprintf(p, "-\t") : sprintf(p, "%u\t", m->hops);
    } else {
        p += sprintf(p, "\t-\t-\t-\t");
    }
    if (m->emoji)
        *p++ = 'e';
    if (m->via_mqtt)
        *p++ = 'm';
    if (!m->emoji && !m->via_mqtt)
        *p++ = '-';
    *p++ = '\t';
    for (const char *t = m->text; *t && t < m->text + MESH_MSG_TEXT_MAX; t++) {
        switch (*t) {
        case '\\': *p++ = '\\'; *p++ = '\\'; break;
        case '\t': *p++ = '\\'; *p++ = 't'; break;
        case '\n': *p++ = '\\'; *p++ = 'n'; break;
        case '\r': *p++ = '\\'; *p++ = 'r'; break;
        default: *p++ = *t; break;
        }
    }
    *p++ = '\n';
    *p = '\0';
    return (size_t)(p - buf);
}

static bool parse_u32(const char *s, int base, uint32_t *out) {
    char *end;
    if (!*s)
        return false;
    unsigned long v = strtoul(s, &end, base);
    if (*end || v > 0xFFFFFFFFul)
        return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_node(const char *s, uint32_t *out) {
    return s[0] == '!' && strlen(s) == 9 && parse_u32(s + 1, 16, out);
}

static bool parse_time(const char *s, uint32_t *out) {
    if (strcmp(s, "-") == 0) {
        *out = 0;
        return true;
    }
    int y;
    unsigned mo, d, h, mi, se;
    char z;
    if (sscanf(s, "%4d-%2u-%2uT%2u:%2u:%2u%c", &y, &mo, &d, &h, &mi, &se, &z) != 7 ||
        z != 'Z' || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60)
        return false;
    int32_t days = days_from_civil(y, mo, d);
    if (days < 0)
        return false;
    *out = (uint32_t)days * 86400 + h * 3600 + mi * 60 + se;
    return true;
}

bool msglog_parse(char *ln, mesh_msg_t *m, uint32_t *n, size_t *status_off) {
    enum { N, TIME, FROM, TO, CH, ID, REPLY, STATUS, ERR, SNR, RSSI, HOPS, FLAGS, TEXT, NF };
    char *f[NF];
    size_t len = strlen(ln);
    if (len && ln[len - 1] == '\n')
        ln[--len] = '\0';
    f[0] = ln;
    for (int i = 1; i < NF; i++) {
        char *tab = strchr(f[i - 1], '\t');
        if (!tab)
            return false;
        *tab = '\0';
        f[i] = tab + 1;
    }

    memset(m, 0, sizeof *m);
    uint32_t v;
    if (!parse_u32(f[N], 10, n) || !parse_time(f[TIME], &m->rx_time) ||
        !parse_node(f[FROM], &m->from) || !parse_node(f[TO], &m->to) ||
        !parse_u32(f[CH], 10, &v) || v >= MESH_MAX_CHANNELS ||
        !parse_u32(f[ID], 16, &m->id) || !parse_u32(f[REPLY], 16, &m->reply_id))
        return false;
    m->channel = (uint8_t)v;

    // Status: a word, space-padded.
    char *sp = strchr(f[STATUS], ' ');
    if (sp)
        *sp = '\0';
    int st = -1;
    for (size_t i = 0; i < sizeof status_words / sizeof *status_words; i++)
        if (status_words[i] && strcmp(f[STATUS], status_words[i]) == 0)
            st = (int)i;
    if (st < 0 || !parse_u32(f[ERR], 10, &v) || v > 255)
        return false;
    m->status = (uint8_t)st;
    m->error = (uint8_t)v;
    *status_off = (size_t)(f[STATUS] - ln);

    m->hops = 0xFF;
    if (st == MESH_MSG_RECEIVED) {
        char *end;
        m->snr = strtof(f[SNR], &end);
        if (*end)
            return false;
        long r = strtol(f[RSSI], &end, 10);
        if (*end || r < INT16_MIN || r > INT16_MAX)
            return false;
        m->rssi = (int16_t)r;
        if (strcmp(f[HOPS], "-") != 0) {
            if (!parse_u32(f[HOPS], 10, &v) || v > 254)
                return false;
            m->hops = (uint8_t)v;
        }
    }
    m->emoji = strchr(f[FLAGS], 'e') != NULL;
    m->via_mqtt = strchr(f[FLAGS], 'm') != NULL;

    size_t o = 0;
    for (const char *t = f[TEXT]; *t && o < MESH_MSG_TEXT_MAX; t++) {
        char c = *t;
        if (c == '\\' && t[1]) {
            c = *++t;
            c = c == 't' ? '\t' : c == 'n' ? '\n' : c == 'r' ? '\r' : c;
        }
        m->text[o++] = c;
    }
    m->text[o] = '\0';
    m->len = (uint8_t)o;
    return true;
}

// ---- files ------------------------------------------------------------------

// Channel files are named for the channel, not its index, so a history
// follows its channel when the node's channels are reordered:
// ch-<name>-<hash>, the name made filename-safe and the hash covering the
// real name and key. chan_file[] maps today's indexes to them, as the node
// last said (kept on the card in MSGLOG_CHANNELS for the next boot). An index
// we don't know the channel of yet falls back to the old ch<index> name.
static char chan_file[MESH_MAX_CHANNELS][MSGLOG_NAME_MAX + 1];

bool msglog_channel_file(int index, char *buf) {
    if (index < 0 || index >= MESH_MAX_CHANNELS)
        return false;
    const mesh_channel_t *c = &mesh_db.channels[index];
    if (c->role == meshtastic_Channel_Role_DISABLED ||
        (!c->name[0] && !mesh_db.has_lora)) // unnamed primary: name unknown yet
        return false;
    const char *name = mesh_db_channel_name(index);
    uint32_t h = 2166136261u;
    for (const char *p = name; *p; p++)
        h = (h ^ (uint8_t)*p) * 16777619u;
    for (int i = 0; i < 4; i++)
        h = (h ^ (uint8_t)(c->psk_hash >> (8 * i))) * 16777619u;
    char safe[sizeof c->name];
    size_t n = 0;
    for (const char *p = name; *p && n < sizeof safe - 1; p++) {
        char ch = *p;
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        safe[n++] = ok ? ch : '_';
    }
    safe[n] = '\0';
    if (n)
        snprintf(buf, MSGLOG_NAME_MAX + 1, "ch-%s-%08lx", safe, (unsigned long)h);
    else
        snprintf(buf, MSGLOG_NAME_MAX + 1, "ch-%08lx", (unsigned long)h);
    return true;
}

// The file a conversation is logged to, without directory or .tsv.
static void conv_name(mesh_conv_t c, char *buf) {
    if (c.peer)
        snprintf(buf, MSGLOG_NAME_MAX + 1, "dm-%08lx", (unsigned long)c.peer);
    else if (c.channel < MESH_MAX_CHANNELS && chan_file[c.channel][0])
        strcpy(buf, chan_file[c.channel]);
    else
        snprintf(buf, MSGLOG_NAME_MAX + 1, "ch%u", c.channel);
}

static void name_path(const char *name, char *buf) {
    snprintf(buf, MSGLOG_PATH_MAX, MSGLOG_DIR "/%s.tsv", name);
}

// Which conversation a file in MSGLOG_DIR holds; false if none we restore.
static bool path_conv(const char *fname, mesh_conv_t *c, char *name) {
    size_t len = strlen(fname);
    if (len < 5 || len - 4 > MSGLOG_NAME_MAX || strcasecmp(fname + len - 4, ".tsv") != 0)
        return false;
    memcpy(name, fname, len - 4);
    name[len - 4] = '\0';
    uint32_t v;
    if (strncasecmp(name, "dm-", 3) == 0 && strlen(name) == 11 &&
        parse_u32(name + 3, 16, &v) && v) {
        *c = MESH_CONV_DM(v);
        return true;
    }
    for (int i = 0; i < MESH_MAX_CHANNELS; i++) {
        if (chan_file[i][0] && strcasecmp(name, chan_file[i]) == 0) {
            *c = MESH_CONV_CHANNEL((uint8_t)i);
            return true;
        }
    }
    // Old-style ch<index>, from before the names: only while that index's
    // channel isn't known.
    if (strncasecmp(name, "ch", 2) == 0 && strlen(name) == 3 &&
        parse_u32(name + 2, 10, &v) && v < MESH_MAX_CHANNELS && !chan_file[v][0]) {
        *c = MESH_CONV_CHANNEL((uint8_t)v);
        return true;
    }
    return false;
}

// Where the last `want` lines of the open file begin, and where its last line
// begins (both 0 for a short file).
static FRESULT tail_scan(int want, FSIZE_t *start, FSIZE_t *last) {
    char buf[256];
    UINT got;
    FSIZE_t pos = f_size(&fil);
    *start = *last = 0;
    if (!pos)
        return FR_OK;
    FRESULT r = f_lseek(&fil, pos - 1);
    if (r == FR_OK)
        r = f_read(&fil, buf, 1, &got);
    if (r != FR_OK)
        return r;
    if (got == 1 && buf[0] == '\n')
        pos--; // the last line's own terminator
    bool have_last = false;
    int lines = 0;
    while (pos > 0) {
        UINT n = pos > sizeof buf ? sizeof buf : (UINT)pos;
        pos -= n;
        if ((r = f_lseek(&fil, pos)) != FR_OK || (r = f_read(&fil, buf, n, &got)) != FR_OK)
            return r;
        for (int i = (int)got - 1; i >= 0; i--) {
            if (buf[i] != '\n')
                continue;
            if (!have_last) {
                *last = pos + (FSIZE_t)i + 1;
                have_last = true;
            }
            if (++lines == want) {
                *start = pos + (FSIZE_t)i + 1;
                return FR_OK;
            }
        }
    }
    return FR_OK;
}

// Buffered line reader over fil, for restore.
static struct {
    char buf[256];
    UINT have;
    FSIZE_t pos; // file offset of buf[0]
    bool valid;
} rd;

// Read the line starting at *pos into line[] and move *pos past it; false at
// end of file or on an error (*res). Overlong lines come back truncated, and
// then fail to parse.
static bool read_line(FSIZE_t *pos, FRESULT *res) {
    size_t len = 0;
    bool any = false;
    *res = FR_OK;
    for (;;) {
        if (!rd.valid || *pos < rd.pos || *pos >= rd.pos + rd.have) {
            rd.pos = *pos;
            rd.valid = (*res = f_lseek(&fil, *pos)) == FR_OK &&
                       (*res = f_read(&fil, rd.buf, sizeof rd.buf, &rd.have)) == FR_OK &&
                       rd.have;
            if (!rd.valid) {
                line[len] = '\0';
                return any && *res == FR_OK;
            }
        }
        char c = rd.buf[*pos - rd.pos];
        (*pos)++;
        any = true;
        if (c == '\n') {
            line[len] = '\0';
            return true;
        }
        if (len < MSGLOG_LINE_MAX)
            line[len++] = c;
    }
}

typedef struct {
    mesh_conv_t conv;
    uint32_t last_n;
    FSIZE_t tail;
    char name[MSGLOG_NAME_MAX + 1];
} file_ent_t;

static FRESULT scan_file(const char *path, file_ent_t *e) {
    FRESULT r = f_open(&fil, path, FA_READ);
    if (r != FR_OK)
        return r;
    FSIZE_t last;
    r = tail_scan(MESH_MSG_POOL, &e->tail, &last);
    e->last_n = 0;
    if (r == FR_OK) {
        UINT got;
        if ((r = f_lseek(&fil, last)) == FR_OK &&
            (r = f_read(&fil, line, 16, &got)) == FR_OK) {
            line[got] = '\0';
            e->last_n = (uint32_t)strtoul(line, NULL, 10);
        }
    }
    f_close(&fil);
    return r;
}

static FRESULT restore_file(const file_ent_t *e, uint32_t *count) {
    char path[MSGLOG_PATH_MAX];
    name_path(e->name, path);
    FRESULT r = f_open(&fil, path, FA_READ | FA_WRITE);
    if (r != FR_OK)
        return r;
    rd.valid = false;
    FSIZE_t patch[PATCHES];
    int patches = 0;
    FSIZE_t pos = e->tail, start = pos;
    while (read_line(&pos, &r)) {
        mesh_msg_t m;
        uint32_t n;
        size_t so;
        if (msglog_parse(line, &m, &n, &so)) {
            if (m.status == MESH_MSG_QUEUED || m.status == MESH_MSG_SENT) {
                m.status = MESH_MSG_FAILED;
                m.error = meshtastic_Routing_Error_TIMEOUT;
                if (patches < PATCHES)
                    patch[patches++] = start + so;
            }
            if (!e->conv.peer)
                m.channel = e->conv.channel; // it may have had another index then
            if (mesh_msgs_restore(e->conv, &m))
                (*count)++;
        }
        start = pos;
    }
    char st[STATUS_W + 1];
    put_status(st, MESH_MSG_FAILED, meshtastic_Routing_Error_TIMEOUT);
    for (int i = 0; i < patches && r == FR_OK; i++) {
        UINT put;
        if ((r = f_lseek(&fil, patch[i])) == FR_OK)
            r = f_write(&fil, st, STATUS_W, &put);
    }
    FRESULT rc = f_close(&fil);
    return r != FR_OK ? r : rc;
}

static int cmp_last_n(const void *a, const void *b) {
    uint32_t x = ((const file_ent_t *)a)->last_n, y = ((const file_ent_t *)b)->last_n;
    return x < y ? -1 : x > y;
}

// ---- queue --------------------------------------------------------------------

typedef enum { REC_APPEND, REC_STATUS } rec_kind_t;

typedef struct {
    uint32_t peer;
    uint32_t id;
    uint16_t len;        // bytes of payload after this header
    uint16_t status_off; // REC_APPEND: status column offset in the line
    uint8_t channel;
    uint8_t kind;
    uint8_t status;
} rec_t;

static uint8_t queue[QUEUE_BYTES];
static size_t q_head, q_used; // q_head: oldest byte
static uint32_t q_count;      // records queued

static void q_copy_in(size_t at, const void *src, size_t n) {
    at %= QUEUE_BYTES;
    size_t first = n < QUEUE_BYTES - at ? n : QUEUE_BYTES - at;
    memcpy(&queue[at], src, first);
    memcpy(queue, (const uint8_t *)src + first, n - first);
}

static void q_copy_out(size_t at, void *dst, size_t n) {
    at %= QUEUE_BYTES;
    size_t first = n < QUEUE_BYTES - at ? n : QUEUE_BYTES - at;
    memcpy(dst, &queue[at], first);
    memcpy((uint8_t *)dst + first, queue, n - first);
}

static void q_push(const rec_t *h, const char *payload) {
    if (!online || q_used + sizeof *h + h->len > QUEUE_BYTES) {
        stats.dropped++;
        return;
    }
    q_copy_in(q_head + q_used, h, sizeof *h);
    q_copy_in(q_head + q_used + sizeof *h, payload, h->len);
    q_used += sizeof *h + h->len;
    q_count++;
}

// ---- outgoing messages still being acked -------------------------------------

typedef struct {
    uint32_t id;
    mesh_conv_t conv;
    FSIZE_t off; // of its status column
    char name[MSGLOG_NAME_MAX + 1]; // the file it's in
    bool used;
} pending_t;
static pending_t pending[PENDING];
static int pending_next;

static int pending_find(uint32_t id, mesh_conv_t c) {
    for (int i = 0; i < PENDING; i++)
        if (pending[i].used && pending[i].id == id && pending[i].conv.peer == c.peer &&
            pending[i].conv.channel == c.channel)
            return i;
    return -1;
}

static bool status_final(uint8_t s) {
    return s == MESH_MSG_RECEIVED || s == MESH_MSG_DELIVERED || s == MESH_MSG_FAILED;
}

// ---- observer -----------------------------------------------------------------

static void on_added(const mesh_msg_t *m, void *ctx) {
    (void)ctx;
    mesh_conv_t c = mesh_msgs_conv_of(m);
    static char buf[MSGLOG_LINE_MAX + 1];
    size_t so;
    size_t len = msglog_format(m, next_n++, buf, &so);
    rec_t h = {.peer = c.peer, .channel = c.channel, .id = m->id, .kind = REC_APPEND,
               .status = m->status, .len = (uint16_t)len, .status_off = (uint16_t)so};
    q_push(&h, buf);
}

static void on_updated(const mesh_msg_t *m, void *ctx) {
    (void)ctx;
    mesh_conv_t c = mesh_msgs_conv_of(m);
    char st[STATUS_W + 1];
    put_status(st, m->status, m->status == MESH_MSG_FAILED ? m->error : 0);
    rec_t h = {.peer = c.peer, .channel = c.channel, .id = m->id, .kind = REC_STATUS,
               .status = m->status, .len = STATUS_W};
    q_push(&h, st);
}

static const mesh_msgs_observer_t observer = {.added = on_added, .updated = on_updated};

// ---- writing ------------------------------------------------------------------

static FRESULT write_all(const void *p, UINT n) {
    UINT put;
    FRESULT r = f_write(&fil, p, n, &put);
    return r == FR_OK && put != n ? FR_DENIED : r; // short write: card full
}

static FRESULT append(const rec_t *h, const char *text) {
    char name[MSGLOG_NAME_MAX + 1], path[MSGLOG_PATH_MAX];
    mesh_conv_t c = h->peer ? MESH_CONV_DM(h->peer) : MESH_CONV_CHANNEL(h->channel);
    conv_name(c, name);
    name_path(name, path);
    FRESULT r = f_open(&fil, path, FA_OPEN_ALWAYS | FA_READ | FA_WRITE);
    if (r != FR_OK)
        return r;
    FSIZE_t size = f_size(&fil);
    if (!size) {
        r = write_all(HEADER, sizeof HEADER - 1);
    } else {
        // A torn last line (power lost mid-write) gets terminated, so ours
        // starts clean.
        char last = '\n';
        UINT got;
        if ((r = f_lseek(&fil, size - 1)) == FR_OK &&
            (r = f_read(&fil, &last, 1, &got)) == FR_OK && last != '\n')
            r = write_all("\n", 1);
    }
    FSIZE_t at = f_tell(&fil);
    if (r == FR_OK)
        r = write_all(text, h->len);
    FRESULT rc = f_close(&fil);
    if (r == FR_OK)
        r = rc;
    if (r == FR_OK && !status_final(h->status)) {
        pending[pending_next] = (pending_t){
            .id = h->id, .conv = c, .off = at + h->status_off, .used = true};
        strcpy(pending[pending_next].name, name);
        pending_next = (pending_next + 1) % PENDING;
    }
    return r;
}

static FRESULT rewrite_status(const rec_t *h, const char *st) {
    mesh_conv_t c = h->peer ? MESH_CONV_DM(h->peer) : MESH_CONV_CHANNEL(h->channel);
    int i = pending_find(h->id, c);
    if (i < 0)
        return FR_OK; // its line never made it to the card
    char path[MSGLOG_PATH_MAX];
    name_path(pending[i].name, path);
    FRESULT r = f_open(&fil, path, FA_OPEN_EXISTING | FA_READ | FA_WRITE);
    if (r != FR_OK)
        return r;
    // Check it's still a status column (the file could have been edited).
    char old[STATUS_W];
    UINT got;
    if ((r = f_lseek(&fil, pending[i].off)) == FR_OK &&
        (r = f_read(&fil, old, STATUS_W, &got)) == FR_OK && got == STATUS_W &&
        old[9] == '\t' && old[0] >= 'a' && old[0] <= 'z' &&
        (r = f_lseek(&fil, pending[i].off)) == FR_OK)
        r = write_all(st, STATUS_W);
    FRESULT rc = f_close(&fil);
    if (status_final(h->status))
        pending[i].used = false;
    return r != FR_OK ? r : rc;
}

int msglog_flush(void) {
    if (!online || !q_used)
        return 0;
    rec_t h;
    static char payload[MSGLOG_LINE_MAX + 1];
    q_copy_out(q_head, &h, sizeof h);
    q_copy_out(q_head + sizeof h, payload, h.len);
    q_head = (q_head + sizeof h + h.len) % QUEUE_BYTES;
    q_used -= sizeof h + h.len;
    q_count--;
    payload[h.len] = '\0';

    FRESULT r = h.kind == REC_APPEND ? append(&h, payload) : rewrite_status(&h, payload);
    if (r != FR_OK) {
        printf("[sd] write failed (FatFs error %d), logging stopped\n", r);
        stats.errors++;
        stats.dropped++;
        msglog_close();
        return -1;
    }
    stats.written++;
    return 1;
}

bool msglog_pending(void) { return online && q_used; }

// ---- channel map ----------------------------------------------------------------

// MSGLOG_CHANNELS: "<index>\t<file>\t<channel name>" per known channel, under
// a # comment line. Only the first two fields are read back.
static FRESULT load_channels(void) {
    memset(chan_file, 0, sizeof chan_file);
    FRESULT r = f_open(&fil, MSGLOG_CHANNELS, FA_READ);
    if (r == FR_NO_FILE)
        return FR_OK;
    if (r != FR_OK)
        return r;
    rd.valid = false;
    FSIZE_t pos = 0;
    while (read_line(&pos, &r)) {
        char *tab = strchr(line, '\t'), *end;
        if (line[0] == '#' || !tab)
            continue;
        *tab = '\0';
        unsigned long i = strtoul(line, &end, 10);
        char *name = tab + 1;
        if ((tab = strchr(name, '\t')))
            *tab = '\0';
        if (*end || end == line || i >= MESH_MAX_CHANNELS || strlen(name) > MSGLOG_NAME_MAX ||
            strncmp(name, "ch-", 3) != 0 || strpbrk(name, "/\\:"))
            continue;
        strcpy(chan_file[i], name);
    }
    FRESULT rc = f_close(&fil);
    return r != FR_OK ? r : rc;
}

static FRESULT save_channels(void) {
    FRESULT r = f_open(&fil, MSGLOG_CHANNELS, FA_CREATE_ALWAYS | FA_WRITE);
    if (r != FR_OK)
        return r;
    static const char hdr[] = "# index\tfile\tchannel, as the node last said\n";
    r = write_all(hdr, sizeof hdr - 1);
    for (int i = 0; i < MESH_MAX_CHANNELS && r == FR_OK; i++) {
        if (!chan_file[i][0])
            continue;
        int n = snprintf(line, sizeof line, "%d\t%s\t%s\n", i, chan_file[i],
                         mesh_db_channel_name(i));
        r = write_all(line, (UINT)n);
    }
    FRESULT rc = f_close(&fil);
    return r != FR_OK ? r : rc;
}

static bool file_exists(const char *path) {
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK;
}

int msglog_sync_channels(void) {
    if (!online)
        return 0;
    // Anything queued belongs to the files as they were.
    while (msglog_pending())
        if (msglog_flush() < 0)
            return -1;

    // First drop what each changed index held, then load each channel's
    // history, so a swap doesn't load one before the other has made room.
    bool changed[MESH_MAX_CHANNELS] = {false}, any = false;
    FRESULT r = FR_OK;
    for (int i = 0; i < MESH_MAX_CHANNELS && r == FR_OK; i++) {
        char now[MSGLOG_NAME_MAX + 1] = "";
        msglog_channel_file(i, now);
        if (strcmp(now, chan_file[i]) == 0)
            continue;
        changed[i] = any = true;
        mesh_conv_t c = MESH_CONV_CHANNEL((uint8_t)i);
        if (chan_file[i][0]) {
            // Another channel has this index now; what we hold is the old one's.
            mesh_msgs_drop(c);
        } else if (now[0]) {
            // ch<i>.tsv, from before channel names (or written before the
            // node said what this channel is), is this channel's history.
            char legacy[8], old_path[MSGLOG_PATH_MAX], new_path[MSGLOG_PATH_MAX];
            snprintf(legacy, sizeof legacy, "ch%d", i);
            name_path(legacy, old_path);
            name_path(now, new_path);
            if (file_exists(old_path)) {
                if (file_exists(new_path)) {
                    printf("[sd] %s.tsv and %s.tsv both hold channel %d; "
                           "leaving %s.tsv as it is\n", now, legacy, i, legacy);
                    mesh_msgs_drop(c); // restored from the old file
                } else {
                    if ((r = f_rename(old_path, new_path)) != FR_OK)
                        break;
                    printf("[sd] channel %d history: %s.tsv renamed %s.tsv\n", i,
                           legacy, now);
                }
                for (int k = 0; k < PENDING; k++)
                    if (pending[k].used && strcmp(pending[k].name, legacy) == 0)
                        strcpy(pending[k].name, now);
            }
        }
        strcpy(chan_file[i], now);
    }
    for (int i = 0; i < MESH_MAX_CHANNELS && r == FR_OK; i++) {
        mesh_conv_t c = MESH_CONV_CHANNEL((uint8_t)i);
        if (!changed[i] || !chan_file[i][0] || mesh_msgs_conv(c))
            continue;
        file_ent_t e = {.conv = c};
        strcpy(e.name, chan_file[i]);
        char path[MSGLOG_PATH_MAX];
        name_path(e.name, path);
        uint32_t count = 0;
        if (file_exists(path) && ((r = scan_file(path, &e)) != FR_OK ||
                                  (r = restore_file(&e, &count)) != FR_OK))
            break;
        stats.restored += count;
    }
    if (r == FR_OK && any)
        r = save_channels();
    if (r != FR_OK) {
        printf("[sd] channel files: FatFs error %d, logging stopped\n", r);
        stats.errors++;
        msglog_close();
        return -1;
    }
    return any;
}

// ---- open / close ---------------------------------------------------------------

int msglog_open(bool restore) {
    msglog_close();
    FRESULT r = f_mkdir(MSGLOG_ROOT);
    if (r == FR_OK || r == FR_EXIST)
        r = f_mkdir(MSGLOG_DIR);
    if (r != FR_OK && r != FR_EXIST) {
        printf("[sd] can't create " MSGLOG_DIR " (FatFs error %d)\n", r);
        return -1;
    }
    if ((r = load_channels()) != FR_OK) {
        printf("[sd] can't read " MSGLOG_CHANNELS " (FatFs error %d)\n", r);
        return -1;
    }

    // Keep the most recently active channel and DM files: the store can't
    // hold more conversations than that anyway.
    static file_ent_t files[FILES];
    int nfiles = 0, dms = 0;
    uint32_t max_n = 0;
    DIR dir;
    FILINFO fno;
    if ((r = f_opendir(&dir, MSGLOG_DIR)) != FR_OK)
        return -1;
    while ((r = f_readdir(&dir, &fno)) == FR_OK && fno.fname[0]) {
        file_ent_t e;
        if ((fno.fattrib & AM_DIR) || !path_conv(fno.fname, &e.conv, e.name))
            continue;
        char path[MSGLOG_PATH_MAX];
        name_path(e.name, path);
        if ((r = scan_file(path, &e)) != FR_OK)
            break;
        if (e.last_n > max_n)
            max_n = e.last_n;
        int slot = nfiles;
        if (e.conv.peer && dms == MESH_MAX_DMS) {
            // Full: replace the quietest DM, if this one is newer.
            slot = -1;
            for (int i = 0; i < nfiles; i++)
                if (files[i].conv.peer &&
                    (slot < 0 || files[i].last_n < files[slot].last_n))
                    slot = i;
            if (files[slot].last_n >= e.last_n)
                continue;
        } else if (nfiles < FILES) {
            nfiles++;
            dms += e.conv.peer != 0;
        } else {
            continue;
        }
        files[slot] = e;
    }
    f_closedir(&dir);
    if (r != FR_OK) {
        printf("[sd] can't read " MSGLOG_DIR " (FatFs error %d)\n", r);
        return -1;
    }

    uint32_t count = 0;
    if (restore) {
        // Oldest conversation first, so the newest ends up on top.
        qsort(files, (size_t)nfiles, sizeof *files, cmp_last_n);
        for (int i = 0; i < nfiles; i++) {
            if ((r = restore_file(&files[i], &count)) != FR_OK) {
                printf("[sd] restore failed (FatFs error %d)\n", r);
                break;
            }
        }
        stats.restored = count;
    }
    if (next_n <= max_n)
        next_n = max_n + 1;
    memset(pending, 0, sizeof pending);
    mesh_msgs_set_observer(&observer);
    online = true;
    return (int)count;
}

void msglog_close(void) {
    stats.dropped += q_count;
    online = false;
    q_head = q_used = 0;
    q_count = 0;
}

bool msglog_online(void) { return online; }
const msglog_stats_t *msglog_stats(void) { return &stats; }
