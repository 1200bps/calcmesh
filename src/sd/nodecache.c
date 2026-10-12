#include "sd/nodecache.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "mesh/nodedb.h"

#define NODE_LINE_MAX 240
#define FIELDS 17
#define COLUMNS "num\tid\tshort\tlong\thw\trole\tuser\theard\tsnr\thops\tch\tmqtt" \
                "\tlat\tlon\talt\tpos_time\tprec\n"

static FIL fil;
static bool saving;
static int next; // node index the save is up to
static uint32_t saved_version, saving_version, last_save_ms;
static uint32_t (*clock_ms)(void);
static nodecache_stats_t stats;

void nodecache_set_clock(uint32_t (*now_ms)(void)) { clock_ms = now_ms; }
const nodecache_stats_t *nodecache_stats(void) { return &stats; }
static uint32_t now(void) { return clock_ms ? clock_ms() : 0; }

void nodecache_clear(void) {
    if (saving)
        f_close(&fil);
    saving = false;
    next = 0;
    saved_version = saving_version = last_save_ms = 0;
    memset(&stats, 0, sizeof stats);
}

// --- loading -------------------------------------------------------------------

// Split at tabs, in place; empty fields count.
static int split(char *s, char **f, int max) {
    int n = 0;
    while (n < max) {
        f[n++] = s;
        char *t = strchr(s, '\t');
        if (!t)
            break;
        *t = '\0';
        s = t + 1;
    }
    return n;
}

static void copy_field(char *dst, size_t size, const char *src) {
    snprintf(dst, size, "%s", strcmp(src, "-") == 0 ? "" : src);
}

// The header says whose view this is; 0 if it doesn't.
static uint32_t owner_of(const char *ln) {
    const char *bang = strstr(ln, "through !");
    return ln[0] == '#' && bang ? (uint32_t)strtoul(bang + 9, NULL, 16) : 0;
}

static bool parse_node(char *ln, mesh_node_t *n) {
    char *f[FIELDS];
    int nf = split(ln, f, FIELDS);
    if (nf < 12)
        return false;
    char *end;
    memset(n, 0, sizeof *n);
    n->num = (uint32_t)strtoul(f[0], &end, 16);
    if (end == f[0] || *end)
        return false;
    copy_field(n->id, sizeof n->id, f[1]);
    if (!n->id[0])
        snprintf(n->id, sizeof n->id, "!%08lx", (unsigned long)n->num);
    copy_field(n->short_name, sizeof n->short_name, f[2]);
    copy_field(n->long_name, sizeof n->long_name, f[3]);
    n->hw_model = (uint16_t)strtoul(f[4], NULL, 10);
    n->role = (uint8_t)strtoul(f[5], NULL, 10);
    n->has_user = f[6][0] == '1';
    n->last_heard = (uint32_t)strtoul(f[7], NULL, 10);
    n->snr = strtof(f[8], NULL);
    n->hops_away = strcmp(f[9], "-") == 0 ? MESH_HOPS_UNKNOWN : (uint8_t)strtoul(f[9], NULL, 10);
    n->channel = (uint8_t)strtoul(f[10], NULL, 10);
    n->via_mqtt = f[11][0] == '1';
    n->battery = MESH_BATTERY_UNKNOWN;
    if (nf >= 17 && strcmp(f[12], "-") != 0 && strcmp(f[13], "-") != 0) {
        n->has_position = true;
        n->latitude_i = (int32_t)strtol(f[12], NULL, 10);
        n->longitude_i = (int32_t)strtol(f[13], NULL, 10);
        n->altitude = (int32_t)strtol(f[14], NULL, 10);
        n->position_time = (uint32_t)strtoul(f[15], NULL, 10);
        n->precision_bits = (uint8_t)strtoul(f[16], NULL, 10);
    }
    return true;
}

static FRESULT read_file(const char *path, int *restored) {
    FRESULT r = f_open(&fil, path, FA_READ);
    if (r != FR_OK)
        return r;
    char buf[128], ln[NODE_LINE_MAX + 1];
    size_t len = 0;
    bool overlong = false, header = true, ours = true;
    UINT got;
    static mesh_node_t n; // ~110 bytes
    while ((r = f_read(&fil, buf, sizeof buf, &got)) == FR_OK && got) {
        for (UINT i = 0; i < got; i++) {
            if (buf[i] != '\n') {
                if (len < NODE_LINE_MAX)
                    ln[len++] = buf[i];
                else
                    overlong = true;
                continue;
            }
            ln[len] = '\0';
            if (header) {
                // Another radio's view of the mesh: none of it is ours
                header = false;
                uint32_t owner = owner_of(ln);
                if (owner && mesh_db.nodes_owner && owner != mesh_db.nodes_owner)
                    ours = false;
                else if (owner && !mesh_db.nodes_owner)
                    mesh_db.nodes_owner = owner; // checked again at the handshake
            } else if (ours && !overlong && ln[0] != '#' && parse_node(ln, &n) &&
                       mesh_db_restore_node(&n)) {
                (*restored)++;
            }
            len = 0;
            overlong = false;
        }
        if (!ours)
            break;
    }
    // A last line without its newline was cut short: leave it
    f_close(&fil);
    return r;
}

int nodecache_load(void) {
    // nodes.tsv is the real one; with only nodes.new, power was lost between
    // removing the old file and renaming the new one, and it's whole.
    int restored = 0;
    FRESULT r = read_file(NODECACHE_PATH, &restored);
    if (r == FR_NO_FILE)
        r = read_file(NODECACHE_TMP, &restored);
    stats.restored += (uint32_t)restored;
    // What we just read back needn't be written straight out again
    saved_version = mesh_db.nodes_version;
    if (r == FR_NO_FILE || r == FR_NO_PATH)
        return 0;
    if (r != FR_OK) {
        stats.errors++;
        return -1;
    }
    return restored;
}

// --- saving --------------------------------------------------------------------

bool nodecache_pending(void) {
    if (saving)
        return true;
    return mesh_db.nodes_owner && mesh_db.node_count > 1 &&
           mesh_db.nodes_version != saved_version && now() - last_save_ms >= NODECACHE_SAVE_MS;
}

// Names are the nodes' own: keep tabs and line breaks out of the columns
static void put_text(char **p, char *end, const char *s) {
    if (!*s)
        s = "-";
    for (; *s && *p < end - 1; s++)
        *(*p)++ = (*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s;
    **p = '\0';
}

static void put(char **p, char *end, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void put(char **p, char *end, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*p, (size_t)(end - *p), fmt, ap);
    va_end(ap);
    if (n > 0)
        *p += n < end - *p ? n : end - *p - 1;
}

static int format_node(const mesh_node_t *n, char *buf, size_t size) {
    char *p = buf, *end = buf + size;
    put(&p, end, "%08lx\t", (unsigned long)n->num);
    put_text(&p, end, n->id);
    put(&p, end, "\t");
    put_text(&p, end, n->short_name);
    put(&p, end, "\t");
    put_text(&p, end, n->long_name);
    put(&p, end, "\t%u\t%u\t%d\t%lu\t%.2f\t", n->hw_model, n->role, n->has_user,
        (unsigned long)n->last_heard, (double)n->snr);
    if (n->hops_away == MESH_HOPS_UNKNOWN)
        put(&p, end, "-");
    else
        put(&p, end, "%u", n->hops_away);
    put(&p, end, "\t%u\t%d", n->channel, n->via_mqtt);
    if (n->has_position)
        put(&p, end, "\t%ld\t%ld\t%ld\t%lu\t%u\n", (long)n->latitude_i, (long)n->longitude_i,
            (long)n->altitude, (unsigned long)n->position_time, n->precision_bits);
    else
        put(&p, end, "\t-\t-\t-\t-\t-\n");
    return (int)(p - buf);
}

static FRESULT write_all(const char *s, UINT n) {
    UINT put_n;
    FRESULT r = f_write(&fil, s, n, &put_n);
    return r == FR_OK && put_n != n ? FR_DENIED : r;
}

static int failed(FRESULT r) {
    printf("[sd] can't save " NODECACHE_PATH " (FatFs error %d)\n", r);
    if (saving)
        f_close(&fil);
    saving = false;
    last_save_ms = now(); // try again at the next interval
    stats.errors++;
    return -1;
}

int nodecache_flush(void) {
    FRESULT r;
    char ln[NODE_LINE_MAX + 1];
    if (!saving) {
        if (!nodecache_pending())
            return 0;
        r = f_mkdir("/calcmesh");
        if (r != FR_OK && r != FR_EXIST)
            return failed(r);
        if ((r = f_open(&fil, NODECACHE_TMP, FA_CREATE_ALWAYS | FA_WRITE)) != FR_OK)
            return failed(r);
        saving = true;
        next = 0;
        saving_version = mesh_db.nodes_version;
        int n = snprintf(ln, sizeof ln, "# calcmesh nodes, heard through !%08lx\n",
                         (unsigned long)mesh_db.nodes_owner);
        if ((r = write_all(ln, (UINT)n)) != FR_OK ||
            (r = write_all(COLUMNS, sizeof COLUMNS - 1)) != FR_OK)
            return failed(r);
        return 0;
    }
    // The table can change between lines; the next save catches that
    while (next < mesh_db.node_count && mesh_db.nodes[next].num == mesh_db.my_node_num)
        next++;
    if (next < mesh_db.node_count) {
        int n = format_node(&mesh_db.nodes[next++], ln, sizeof ln);
        if ((r = write_all(ln, (UINT)n)) != FR_OK)
            return failed(r);
        return 0;
    }
    saving = false;
    if ((r = f_close(&fil)) != FR_OK)
        return failed(r);
    r = f_unlink(NODECACHE_PATH);
    if (r == FR_OK || r == FR_NO_FILE)
        r = f_rename(NODECACHE_TMP, NODECACHE_PATH);
    if (r != FR_OK)
        return failed(r);
    saved_version = saving_version;
    last_save_ms = now();
    stats.saves++;
    return 0;
}

void nodecache_close(void) {
    if (saving)
        f_close(&fil);
    saving = false;
}
