#include "sd/settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ff.h"

#define LINE_MAX (SETTINGS_KEY_MAX + SETTINGS_VALUE_MAX + 8)

typedef struct {
    char key[SETTINGS_KEY_MAX + 1];
    char value[SETTINGS_VALUE_MAX + 1];
    bool changed; // set in RAM since boot: a load doesn't override it
} entry_t;

static entry_t table[SETTINGS_MAX];
static int count;
static bool dirty;
static uint32_t version, changed_at;
static uint32_t (*clock_ms)(void);
static FIL fil;

void settings_set_clock(uint32_t (*now_ms)(void)) { clock_ms = now_ms; }
uint32_t settings_version(void) { return version; }
bool settings_dirty(void) { return dirty; }
uint32_t settings_changed_at(void) { return changed_at; }

void settings_clear(void) {
    memset(table, 0, sizeof table);
    count = 0;
    dirty = false;
    version++;
}

static entry_t *find(const char *key) {
    for (int i = 0; i < count; i++)
        if (strcmp(table[i].key, key) == 0)
            return &table[i];
    return NULL;
}

static bool key_ok(const char *key) {
    size_t n = strlen(key);
    if (!n || n > SETTINGS_KEY_MAX || key[0] == '#')
        return false;
    for (const char *p = key; *p; p++)
        if ((unsigned char)*p <= ' ' || *p == '=' || *p == 0x7f)
            return false;
    return true;
}

static bool value_ok(const char *v) {
    if (strlen(v) > SETTINGS_VALUE_MAX)
        return false;
    for (const char *p = v; *p; p++)
        if (*p == '\n' || *p == '\r')
            return false;
    return true;
}

const char *settings_get(const char *key) {
    entry_t *e = find(key);
    return e ? e->value : NULL;
}

int32_t settings_get_int(const char *key, int32_t def) {
    const char *v = settings_get(key);
    if (!v || !*v)
        return def;
    char *end;
    long n = strtol(v, &end, 0);
    return *end || n < INT32_MIN || n > INT32_MAX ? def : (int32_t)n;
}

bool settings_get_bool(const char *key, bool def) {
    static const char *const yes[] = {"1", "on", "yes", "true"};
    static const char *const no[] = {"0", "off", "no", "false"};
    const char *v = settings_get(key);
    if (!v)
        return def;
    for (size_t i = 0; i < sizeof yes / sizeof *yes; i++) {
        if (strcasecmp(v, yes[i]) == 0)
            return true;
        if (strcasecmp(v, no[i]) == 0)
            return false;
    }
    return def;
}

// Store without marking it changed (for loading).
static entry_t *put(const char *key, const char *value) {
    entry_t *e = find(key);
    if (!e) {
        if (count == SETTINGS_MAX)
            return NULL;
        e = &table[count++];
        memset(e, 0, sizeof *e);
        strcpy(e->key, key);
    }
    strcpy(e->value, value);
    return e;
}

bool settings_set(const char *key, const char *value) {
    if (!key_ok(key) || !value_ok(value))
        return false;
    entry_t *e = find(key);
    if (e && strcmp(e->value, value) == 0)
        return true;
    if (!(e = put(key, value)))
        return false;
    e->changed = true;
    dirty = true;
    changed_at = clock_ms ? clock_ms() : 0;
    version++;
    return true;
}

bool settings_set_int(const char *key, int32_t value) {
    char buf[12];
    snprintf(buf, sizeof buf, "%ld", (long)value);
    return settings_set(key, buf);
}

bool settings_set_bool(const char *key, bool value) {
    return settings_set(key, value ? "1" : "0");
}

// ---- the file -----------------------------------------------------------------

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = '\0';
    return s;
}

static void parse_line(char *ln) {
    ln = trim(ln);
    if (!*ln || *ln == '#')
        return;
    char *eq = strchr(ln, '=');
    if (!eq)
        return;
    *eq = '\0';
    char *key = trim(ln), *value = trim(eq + 1);
    if (!key_ok(key) || !value_ok(value))
        return;
    entry_t *e = find(key);
    if (e && e->changed)
        return; // set since boot: ours wins
    put(key, value);
}

static FRESULT read_file(const char *path) {
    FRESULT r = f_open(&fil, path, FA_READ);
    if (r != FR_OK)
        return r;
    char buf[128], ln[LINE_MAX + 1];
    size_t len = 0;
    bool overlong = false;
    UINT got;
    while ((r = f_read(&fil, buf, sizeof buf, &got)) == FR_OK && got) {
        for (UINT i = 0; i < got; i++) {
            if (buf[i] == '\n') {
                ln[len] = '\0';
                if (!overlong)
                    parse_line(ln);
                len = 0;
                overlong = false;
            } else if (len < LINE_MAX) {
                ln[len++] = buf[i];
            } else {
                overlong = true;
            }
        }
    }
    if (r == FR_OK && len && !overlong) {
        ln[len] = '\0';
        parse_line(ln); // no newline at the end
    }
    f_close(&fil);
    return r;
}

int settings_load(void) {
    // settings.txt is the real one. With only settings.new, power was lost
    // between removing the old file and renaming the new one: it's whole.
    FRESULT r = read_file(SETTINGS_PATH);
    if (r == FR_NO_FILE) {
        r = read_file(SETTINGS_TMP);
        if (r == FR_OK)
            r = f_rename(SETTINGS_TMP, SETTINGS_PATH);
    }
    version++;
    return r == FR_NO_FILE || r == FR_NO_PATH ? FR_OK : r;
}

static FRESULT write_all(const char *s) {
    UINT n = (UINT)strlen(s), put;
    FRESULT r = f_write(&fil, s, n, &put);
    return r == FR_OK && put != n ? FR_DENIED : r;
}

int settings_save(void) {
    if (!dirty)
        return FR_OK;
    FRESULT r = f_mkdir("/calcmesh");
    if (r != FR_OK && r != FR_EXIST)
        return r;
    if ((r = f_open(&fil, SETTINGS_TMP, FA_CREATE_ALWAYS | FA_WRITE)) != FR_OK)
        return r;
    r = write_all("# calcmesh settings: key = value, one per line\n");
    for (int i = 0; i < count && r == FR_OK; i++)
        if ((r = write_all(table[i].key)) == FR_OK && (r = write_all(" = ")) == FR_OK &&
            (r = write_all(table[i].value)) == FR_OK)
            r = write_all("\n");
    FRESULT rc = f_close(&fil);
    if (r == FR_OK)
        r = rc;
    if (r == FR_OK) {
        r = f_unlink(SETTINGS_PATH);
        if (r == FR_OK || r == FR_NO_FILE)
            r = f_rename(SETTINGS_TMP, SETTINGS_PATH);
    }
    if (r == FR_OK)
        dirty = false;
    return r;
}
