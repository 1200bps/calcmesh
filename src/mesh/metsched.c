#include "metsched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link.h"
#include "nodedb.h"
#include "sd/metlog.h"
#include "sd/settings.h"

static met_sched_t tab[METSCHED_MAX];
static int count;
static uint32_t seen_version; // settings_version() as of our last read or write

// The run in progress
static uint32_t cur_node;      // 0: none
static uint8_t cur_pending;    // kinds not yet answered or given up on
static uint8_t cur_retry;      // of those, the ones to ask once more
static uint32_t retry_at[MET_KINDS]; // when, link clock
static met_kind_t cur_kind = MET_KINDS; // asking for now
static bool cur_inflight;      // our request is the one metrics.c is on
static uint32_t cur_seq;       // its mesh_metrics_req_seq()
static uint32_t gap_until;     // link clock

static const uint16_t steps[] = {30, 60, 120, 180, 360, 720, 1440};

static bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

// --- kinds ---------------------------------------------------------------------

static const char letters[MET_KINDS] = {
    [MET_DEVICE] = 'd', [MET_ENV] = 'e', [MET_POWER] = 'p', [MET_LOCAL] = 'l'};

const char *metsched_kinds_str(uint8_t kinds, char buf[MET_KINDS + 1]) {
    char *p = buf;
    for (int k = 0; k < MET_KINDS; k++)
        if (kinds & MET_BIT(k))
            *p++ = letters[k];
    *p = 0;
    return buf;
}

bool metsched_parse_kinds(const char *s, uint8_t *kinds) {
    uint8_t m = 0;
    for (; *s; s++) {
        const char *l = memchr(letters, *s | 0x20, MET_KINDS);
        if (!l)
            return false;
        m |= (uint8_t)MET_BIT(l - letters);
    }
    *kinds = m;
    return m != 0;
}

uint16_t metsched_step(uint16_t every_min, int dir) {
    int n = (int)(sizeof steps / sizeof steps[0]), i = 0;
    while (i < n - 1 && steps[i] < every_min)
        i++;
    // every_min between two steps: down goes to the lower one
    if (dir < 0 && steps[i] > every_min && i > 0)
        return steps[i - 1];
    i += dir;
    return steps[i < 0 ? 0 : i >= n ? n - 1 : i];
}

static uint16_t clamp_every(long m) {
    return (uint16_t)(m < METSCHED_MIN_MIN ? METSCHED_MIN_MIN
                      : m > METSCHED_MAX_MIN ? METSCHED_MAX_MIN
                                             : m);
}

// --- the table -------------------------------------------------------------------

int metsched_count(void) { return count; }
const met_sched_t *metsched_at(int i) { return i >= 0 && i < count ? &tab[i] : NULL; }

static met_sched_t *find(uint32_t node) {
    for (int i = 0; i < count; i++)
        if (tab[i].node == node)
            return &tab[i];
    return NULL;
}

const met_sched_t *metsched_find(uint32_t node) { return node ? find(node) : NULL; }

static void slot_key(int i, char key[32]) { snprintf(key, 32, SET_METRICS_SCHED "%d", i + 1); }

static void save(void) {
    char key[32], val[32], kb[MET_KINDS + 1];
    for (int i = 0; i < METSCHED_MAX; i++) {
        slot_key(i, key);
        if (i < count)
            snprintf(val, sizeof val, "!%08lx %s %u", (unsigned long)tab[i].node,
                     metsched_kinds_str(tab[i].kinds, kb), tab[i].every_min);
        else
            val[0] = 0;
        // An empty slot that was never set needn't be written
        if (val[0] || settings_get(key))
            settings_set(key, val);
    }
    seen_version = settings_version();
}

// Read the slots, keeping what's running for nodes that are still there.
static void load(void) {
    met_sched_t old[METSCHED_MAX];
    int nold = count;
    memcpy(old, tab, sizeof old);
    count = 0;
    for (int i = 0; i < METSCHED_MAX; i++) {
        char key[32], buf[SETTINGS_VALUE_MAX + 1];
        slot_key(i, key);
        const char *v = settings_get(key);
        if (!v || !*v)
            continue;
        snprintf(buf, sizeof buf, "%s", v);
        char *save_p, *node = strtok_r(buf, " \t", &save_p);
        char *kinds = strtok_r(NULL, " \t", &save_p);
        char *every = strtok_r(NULL, " \t", &save_p);
        met_sched_t s = {0};
        char *end;
        if (!node || !kinds || !every || node[0] != '!')
            continue;
        s.node = (uint32_t)strtoul(node + 1, &end, 16);
        if (*end || !s.node || !metsched_parse_kinds(kinds, &s.kinds))
            continue;
        long m = strtol(every, &end, 10);
        if (*end && *end != 'm')
            continue;
        s.every_min = clamp_every(m);
        if (find(s.node))
            continue; // a node twice: the first slot wins
        for (int j = 0; j < nold; j++)
            if (old[j].node == s.node) {
                met_sched_t keep = old[j];
                keep.kinds = s.kinds;
                keep.every_min = s.every_min;
                s = keep;
            }
        tab[count++] = s;
    }
    if (cur_node && !find(cur_node))
        cur_node = 0;
    seen_version = settings_version();
}

// When the next run comes after a change: at once for a new one, else the
// new interval after the last run's start
static void place_changed(met_sched_t *s, uint32_t now) {
    s->placed = true;
    uint32_t next = s->started_ms + s->every_min * 60000u;
    s->next_ms = s->ran && !reached(now, next) ? next : now;
}

bool metsched_set(uint32_t node, uint8_t kinds, uint16_t every_min) {
    if (!node)
        return false;
    met_sched_t *s = find(node);
    kinds &= (uint8_t)(MET_BIT(MET_KINDS) - 1);
    if (!kinds) {
        if (s) {
            if (cur_node == node)
                cur_node = 0;
            int i = (int)(s - tab);
            memmove(&tab[i], &tab[i + 1], (size_t)(count - i - 1) * sizeof tab[0]);
            count--;
            save();
        }
        return true;
    }
    if (!s) {
        if (count == METSCHED_MAX)
            return false;
        s = &tab[count++];
        memset(s, 0, sizeof *s);
        s->node = node;
    }
    uint16_t every = clamp_every(every_min);
    bool changed = !s->placed || s->every_min != every;
    s->kinds = kinds;
    s->every_min = every;
    if (changed)
        place_changed(s, mesh_link_ms());
    if (cur_node == node) {
        cur_pending &= kinds;
        cur_retry &= kinds;
    }
    save();
    return true;
}

// --- running -------------------------------------------------------------------

bool metsched_running(uint32_t node, met_kind_t *kind, int *try_no) {
    if (!cur_node || cur_node != node)
        return false;
    if (kind)
        *kind = cur_inflight ? cur_kind : MET_KINDS;
    if (try_no)
        *try_no = cur_inflight && (cur_retry & MET_BIT(cur_kind)) ? 2 : 1;
    return true;
}

uint32_t metsched_due_in_ms(const met_sched_t *s) {
    if (!s->placed || s->node == cur_node)
        return 0;
    uint32_t now = mesh_link_ms();
    return reached(now, s->next_ms) ? 0 : s->next_ms - now;
}

static void finish(met_sched_t *s, uint32_t now) {
    s->last_epoch = mesh_link_epoch();
    s->next_ms = s->started_ms + s->every_min * 60000u;
    if (reached(now, s->next_ms)) // a run that took that long: an interval from now
        s->next_ms = now + s->every_min * 60000u;
    cur_node = 0;
}

static void step(met_sched_t *s, uint32_t now) {
    if (cur_inflight) {
        // Someone asked by hand meanwhile, replacing ours: ask again later,
        // not counting this try
        if (mesh_metrics_req_seq() != cur_seq) {
            cur_inflight = false;
            return;
        }
        met_req_state_t st = mesh_metrics_req_state();
        if (st == MET_REQ_WAITING)
            return;
        cur_inflight = false;
        gap_until = now + METSCHED_GAP_MS;
        uint8_t bit = (uint8_t)MET_BIT(cur_kind);
        if (st == MET_REQ_DONE) {
            s->last_ok |= bit;
        } else if (mesh_metrics_req_error() == MET_ERR_NO_ANSWER && !(cur_retry & bit)) {
            // Once more, later; the other kinds meanwhile
            cur_retry |= bit;
            retry_at[cur_kind] = now + METSCHED_RETRY_MS;
            cur_kind = MET_KINDS;
            return;
        } else {
            uint8_t err = mesh_metrics_req_error();
            s->last_failed |= bit;
            s->last_error[cur_kind] = err;
            metlog_failed(s->node, cur_kind, err, mesh_link_epoch());
        }
        cur_pending &= (uint8_t)~bit;
        cur_retry &= (uint8_t)~bit;
        cur_kind = MET_KINDS;
        return;
    }
    if (!cur_pending) {
        finish(s, now);
        return;
    }
    // A request made by hand goes first
    if (!reached(now, gap_until) || mesh_metrics_req_state() == MET_REQ_WAITING)
        return;
    // Kinds not asked yet first, then those due to be asked again
    met_kind_t k = MET_KINDS;
    for (met_kind_t i = 0; i < MET_KINDS && k == MET_KINDS; i++)
        if ((cur_pending & ~cur_retry) & MET_BIT(i))
            k = i;
    for (met_kind_t i = 0; i < MET_KINDS && k == MET_KINDS; i++)
        if ((cur_retry & MET_BIT(i)) && reached(now, retry_at[i]))
            k = i;
    if (k == MET_KINDS)
        return;
    uint32_t wait = s->node == mesh_db.my_node_num ? MET_REQ_LOCAL_MS : METSCHED_WAIT_MS;
    if (!mesh_metrics_request_wait(s->node, k, wait)) {
        gap_until = now + METSCHED_GAP_MS; // no link after all: try again
        return;
    }
    cur_kind = k;
    cur_seq = mesh_metrics_req_seq();
    cur_inflight = true;
}

void metsched_task(void) {
    if (settings_version() != seen_version)
        load();
    if (mesh_link_state() != MESH_LINK_READY || !mesh_db.my_node_num)
        return;
    uint32_t now = mesh_link_ms();
    for (int i = 0; i < count; i++)
        if (!tab[i].placed) {
            tab[i].placed = true;
            tab[i].next_ms = now + METSCHED_FIRST_MS;
        }
    met_sched_t *s = cur_node ? find(cur_node) : NULL;
    if (s) {
        step(s, now);
        return;
    }
    cur_node = 0;
    // The one most overdue
    for (int i = 0; i < count; i++)
        if (reached(now, tab[i].next_ms) && (!s || (int32_t)(tab[i].next_ms - s->next_ms) < 0))
            s = &tab[i];
    if (!s)
        return;
    cur_node = s->node;
    cur_pending = s->kinds;
    cur_retry = 0;
    cur_inflight = false;
    cur_kind = MET_KINDS;
    s->started_ms = now;
    s->ran = true;
    s->last_ok = s->last_failed = 0;
    memset(s->last_error, 0, sizeof s->last_error);
    gap_until = now;
    step(s, now);
}

// --- logging -------------------------------------------------------------------

static void on_add(const mesh_metrics_t *e, met_kind_t kind, uint32_t time) {
    if (!find(e->num))
        return;
    bool asked = mesh_metrics_req_state() == MET_REQ_WAITING && mesh_metrics_req_node() == e->num &&
                 mesh_metrics_req_kind() == kind;
    // Our own node hands us device metrics every minute: only what we asked for
    if (!asked && e->num == mesh_db.my_node_num)
        return;
    metlog_telemetry(e, kind, time ? time : mesh_link_epoch(), asked);
}

void metsched_init(void) {
    mesh_metrics_on_add(on_add);
    load();
}

// --- console -------------------------------------------------------------------

void metsched_print(void) {
    if (!count) {
        printf("no scheduled metrics\n");
        return;
    }
    for (int i = 0; i < count; i++) {
        const met_sched_t *s = &tab[i];
        char kb[MET_KINDS + 1];
        printf("!%08lx %-4s every %u min, ", (unsigned long)s->node,
               metsched_kinds_str(s->kinds, kb), s->every_min);
        met_kind_t k;
        int t;
        if (metsched_running(s->node, &k, &t))
            printf("running%s%s", k < MET_KINDS ? ": asking for " : "",
                   k < MET_KINDS ? mesh_metrics_kind_name(k) : "");
        else if (!s->placed)
            printf("first run once the node is connected");
        else
            printf("next in %lu min", (unsigned long)(metsched_due_in_ms(s) + 59999) / 60000);
        if (s->last_epoch)
            printf("; last run got %s",
                   s->last_ok ? metsched_kinds_str(s->last_ok, kb) : "nothing");
        printf("\n");
    }
}
