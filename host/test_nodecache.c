// The node list on the SD card (src/sd/nodecache.c), on a FAT-formatted RAM
// disk: saved a line at a time, restored as cached, only for the same radio.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "mesh/nodedb.h"
#include "ramdisk.h"
#include "sd/nodecache.h"

static int failures;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
            failures++;                                                        \
        }                                                                      \
    } while (0)

#define ME 0xa1b2c3d4u

// ramdisk.c's get_fattime() wants it; msglog.c isn't in this test.
uint32_t msglog_fattime(uint32_t epoch);
uint32_t msglog_fattime(uint32_t epoch) {
    (void)epoch;
    return (uint32_t)(2026 - 1980) << 25 | 1u << 21 | 1u << 16;
}

static FATFS fs;
static uint32_t clock_ms;
static uint32_t fake_now(void) { return clock_ms; }

static void fresh_card(void) {
    static BYTE work[FF_MAX_SS];
    ramdisk_create(8u << 20);
    MKFS_PARM opt = {.fmt = FM_FAT | FM_SFD};
    CHECK(f_mkfs("", &opt, work, sizeof work) == FR_OK);
    CHECK(f_mount(&fs, "", 1) == FR_OK);
}

static void handshake(uint32_t me) {
    meshtastic_MyNodeInfo mi = meshtastic_MyNodeInfo_init_zero;
    mi.my_node_num = me;
    mesh_db_dump_begin(false);
    mesh_db_set_my_info(&mi);
    meshtastic_NodeInfo ni = meshtastic_NodeInfo_init_zero;
    ni.num = me;
    ni.has_user = true;
    strcpy(ni.user.long_name, "Calcmesh Base");
    strcpy(ni.user.short_name, "12Tc");
    mesh_db_upsert_node(&ni);
}

static void add_user(uint32_t num, const char *lname, const char *sname, bool key) {
    meshtastic_User u = meshtastic_User_init_zero;
    strcpy(u.long_name, lname);
    strcpy(u.short_name, sname);
    u.hw_model = meshtastic_HardwareModel_HELTEC_V4;
    u.role = meshtastic_Config_DeviceConfig_Role_ROUTER;
    u.public_key.size = key ? 32 : 0;
    CHECK(mesh_db_set_user(num, &u) != NULL);
}

static int save_all(void) {
    int steps = 0;
    while (nodecache_pending() && steps < 1000) {
        CHECK(nodecache_flush() == 0);
        steps++;
    }
    return steps;
}

static char *slurp(const char *path) {
    static char buf[8192];
    FIL f;
    UINT got = 0;
    buf[0] = '\0';
    if (f_open(&f, path, FA_READ) != FR_OK)
        return NULL;
    f_read(&f, buf, sizeof buf - 1, &got);
    buf[got] = '\0';
    f_close(&f);
    return buf;
}

static void reboot(void) {
    mesh_db_clear();
    nodecache_clear();
    CHECK(f_mount(NULL, "", 0) == FR_OK);
    CHECK(f_mount(&fs, "", 1) == FR_OK);
}

int main(void) {
    fresh_card();
    nodecache_set_clock(fake_now);
    mesh_db_clear();
    clock_ms = 1000;

    // Nothing saved yet: nothing to restore, and no complaint
    CHECK(nodecache_load() == 0);
    CHECK(!nodecache_pending());

    handshake(ME);
    mesh_node_t *hill = mesh_db_heard(0x11111111, 1700000000, 6.5f, 0, 0, false);
    add_user(0x11111111, "Hilltop\tRouter", "HILL", true); // a tab in a name
    hill->is_favorite = true;
    meshtastic_Position pos = meshtastic_Position_init_zero;
    pos.has_latitude_i = pos.has_longitude_i = true;
    pos.latitude_i = 401234567;
    pos.longitude_i = -751234567;
    pos.has_altitude = true;
    pos.altitude = 321;
    pos.time = 1699999990;
    pos.precision_bits = 13;
    CHECK(mesh_db_set_position(0x11111111, &pos));
    mesh_db_heard(0x44444444, 1700000500, -12.25f, 3, 1, true); // bare: never named

    // Not due until the interval has passed since boot
    CHECK(!nodecache_pending());
    clock_ms += NODECACHE_SAVE_MS;
    CHECK(nodecache_pending());
    // Header, then a line per call for the two other nodes, then the rename
    CHECK(save_all() == 4);
    CHECK(nodecache_stats()->saves == 1);
    CHECK(!nodecache_pending());
    char *t = slurp(NODECACHE_PATH);
    CHECK(t && strncmp(t, "# calcmesh nodes, heard through !a1b2c3d4\n", 42) == 0);
    CHECK(t && strstr(t, "11111111\t!11111111\tHILL\tHilltop Router\t") != NULL);
    CHECK(t && strstr(t, "44444444\t!44444444\t-\t-\t") != NULL);
    CHECK(t && strstr(t, "Calcmesh Base") == NULL); // we aren't kept
    CHECK(slurp(NODECACHE_TMP) == NULL);

    // Nothing changed: no save, however long it's been
    clock_ms += NODECACHE_SAVE_MS;
    CHECK(!nodecache_pending());
    // A change waits for the interval
    mesh_db_heard(0x44444444, 1700000600, -11.0f, 3, 1, true);
    CHECK(nodecache_pending());
    CHECK(save_all() == 4);
    clock_ms += 1000;
    mesh_db_heard(0x44444444, 1700000700, -11.0f, 3, 1, true);
    CHECK(!nodecache_pending());

    // Power cycle: they come back cached, before and after the handshake
    reboot();
    CHECK(nodecache_load() == 2);
    CHECK(mesh_db.nodes_owner == ME);
    hill = mesh_db_find(0x11111111);
    CHECK(hill && hill->cached && !hill->has_key && !hill->is_favorite && hill->has_user);
    CHECK(hill && strcmp(hill->long_name, "Hilltop Router") == 0);
    CHECK(hill && strcmp(hill->short_name, "HILL") == 0);
    CHECK(hill && hill->hw_model == meshtastic_HardwareModel_HELTEC_V4 &&
          hill->role == meshtastic_Config_DeviceConfig_Role_ROUTER);
    CHECK(hill && hill->last_heard == 1700000000 && hill->snr == 6.5f && hill->hops_away == 0);
    CHECK(hill && hill->has_position && hill->latitude_i == 401234567 &&
          hill->longitude_i == -751234567 && hill->altitude == 321 &&
          hill->position_time == 1699999990 && hill->precision_bits == 13);
    const mesh_node_t *tro = mesh_db_find(0x44444444);
    CHECK(tro && !tro->has_user && tro->hops_away == 3 && tro->channel == 1 && tro->via_mqtt);
    CHECK(tro && tro->last_heard == 1700000700 - 100); // the second save had 600
    CHECK(tro && tro->short_name[0] == '\0' && tro->long_name[0] == '\0');
    // Just read back, so not written straight out again
    clock_ms += NODECACHE_SAVE_MS;
    CHECK(!nodecache_pending());

    // The handshake: the node lists HILL, so it's live again
    handshake(ME);
    add_user(0x11111111, "Hilltop Router", "HILL", true);
    mesh_db_dump_end();
    CHECK(!hill->cached && hill->has_key);
    CHECK(tro->cached);

    // Loading again (a card swapped in) adds nothing we have
    CHECK(nodecache_load() == 0);

    // A file from another radio isn't ours
    reboot();
    handshake(0x99999999);
    CHECK(nodecache_load() == 0);
    CHECK(mesh_db.node_count == 1);

    // ...and restored before the handshake, the handshake drops it
    reboot();
    CHECK(nodecache_load() == 2);
    handshake(0x99999999);
    CHECK(mesh_db.node_count == 1 && mesh_db_find(0x11111111) == NULL);

    // Power lost between removing the old file and renaming the new one
    reboot();
    CHECK(f_rename(NODECACHE_PATH, NODECACHE_TMP) == FR_OK);
    CHECK(nodecache_load() == 2);

    // A card error partway through a save: dropped, tried again later
    reboot();
    handshake(ME);
    CHECK(nodecache_load() == 2);
    mesh_db_heard(0x55555555, 1700001000, 1.0f, 1, 0, false);
    clock_ms += NODECACHE_SAVE_MS;
    CHECK(nodecache_pending());
    CHECK(nodecache_flush() == 0); // header
    ramdisk_fail(0);
    int r = 0;
    for (int i = 0; i < 10 && r == 0; i++)
        r = nodecache_flush();
    CHECK(r == -1);
    CHECK(nodecache_stats()->errors == 1);
    ramdisk_fail(-1);
    CHECK(!nodecache_pending());
    clock_ms += NODECACHE_SAVE_MS;
    CHECK(nodecache_pending());
    save_all();
    reboot();
    CHECK(nodecache_load() == 3);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
