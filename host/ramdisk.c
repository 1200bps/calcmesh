// FatFs disk driver over a RAM image, for the SD card tests. ramdisk_fail
// makes every access after the next N fail, to stand in for a pulled card.
#include "ramdisk.h"

#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "diskio.h"
#include "sd/msglog.h"

#define SECTOR 512

static uint8_t *image;
static LBA_t sectors;
static long fail_after = -1;
uint32_t ramdisk_epoch;

void ramdisk_create(uint32_t bytes) {
    free(image);
    sectors = bytes / SECTOR;
    image = calloc(sectors, SECTOR);
    fail_after = -1;
}

void ramdisk_fail(long after) { fail_after = after; }

static bool failing(void) {
    if (fail_after < 0)
        return false;
    if (fail_after == 0)
        return true;
    fail_after--;
    return false;
}

DSTATUS disk_initialize(BYTE pdrv) { return pdrv || !image ? STA_NOINIT : 0; }
DSTATUS disk_status(BYTE pdrv) { return pdrv || !image ? STA_NOINIT : 0; }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv || sector + count > sectors)
        return RES_PARERR;
    if (failing())
        return RES_ERROR;
    memcpy(buff, image + (size_t)sector * SECTOR, (size_t)count * SECTOR);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv || sector + count > sectors)
        return RES_PARERR;
    if (failing())
        return RES_ERROR;
    memcpy(image + (size_t)sector * SECTOR, buff, (size_t)count * SECTOR);
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv)
        return RES_PARERR;
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = sectors;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = SECTOR;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

DWORD get_fattime(void) { return msglog_fattime(ramdisk_epoch); }
