// SD card over SPI, as FatFs's disk driver (diskio.h). The PicoCalc wires
// its microSD slot to spi0: GP16 MISO, GP17 CS, GP18 SCK, GP19 MOSI (and
// GP22 card detect, which sd.c watches). SPI mode as in the SD Physical
// Layer spec: CMD0, CMD8 / ACMD41 (or CMD1 for MMC), then 512-byte blocks.
// Everything here blocks; a sector takes ~0.4 ms at 12.5 MHz, but a card can
// sit busy for a few hundred ms after a write.

#include <stdbool.h>
#include <stdint.h>

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/time.h"

#include "ff.h"
#include "diskio.h"
#include "mesh/link.h"
#include "sd/msglog.h"
#include "sd/sd.h"

#define INIT_HZ 400000
#ifndef SD_SPI_HZ
#define SD_SPI_HZ 12500000
#endif

#define CMD0 0    // GO_IDLE_STATE
#define CMD1 1    // SEND_OP_COND (MMC)
#define CMD8 8    // SEND_IF_COND
#define CMD12 12  // STOP_TRANSMISSION
#define CMD16 16  // SET_BLOCKLEN
#define CMD17 17  // READ_SINGLE_BLOCK
#define CMD18 18  // READ_MULTIPLE_BLOCK
#define CMD24 24  // WRITE_BLOCK
#define CMD25 25  // WRITE_MULTIPLE_BLOCK
#define CMD55 55  // APP_CMD
#define CMD58 58  // READ_OCR
#define ACMD23 (0x80 | 23) // SET_WR_BLK_ERASE_COUNT
#define ACMD41 (0x80 | 41) // SD_SEND_OP_COND

#define CT_MMC 0x01
#define CT_SDC1 0x02
#define CT_SDC2 0x04
#define CT_BLOCK 0x08 // block addressing (SDHC/SDXC)

static volatile DSTATUS stat = STA_NOINIT;
static uint8_t card_type;

static uint8_t xchg(uint8_t b) {
    uint8_t r;
    spi_write_read_blocking(SD_SPI, &b, &r, 1);
    return r;
}

static bool wait_ready(uint32_t ms) {
    absolute_time_t until = make_timeout_time_ms(ms);
    do {
        if (xchg(0xFF) == 0xFF)
            return true;
    } while (!time_reached(until));
    return false;
}

static void card_deselect(void) {
    gpio_put(SD_CS_PIN, 1);
    xchg(0xFF); // a clock with CS high makes the card let go of MISO
}

static bool card_select(void) {
    gpio_put(SD_CS_PIN, 0);
    xchg(0xFF);
    if (wait_ready(500))
        return true;
    card_deselect();
    return false;
}

static bool rx_datablock(uint8_t *buf, size_t n) {
    absolute_time_t until = make_timeout_time_ms(200);
    uint8_t token;
    do
        token = xchg(0xFF);
    while (token == 0xFF && !time_reached(until));
    if (token != 0xFE)
        return false;
    spi_read_blocking(SD_SPI, 0xFF, buf, n);
    xchg(0xFF); // CRC, unchecked
    xchg(0xFF);
    return true;
}

static bool tx_datablock(const uint8_t *buf, uint8_t token) {
    if (!wait_ready(500))
        return false;
    xchg(token);
    if (token == 0xFD) // stop tran
        return true;
    spi_write_blocking(SD_SPI, buf, 512);
    xchg(0xFF);
    xchg(0xFF);
    return (xchg(0xFF) & 0x1F) == 0x05; // data accepted
}

// R1 response, or 0xFF if the card never answered.
static uint8_t send_cmd(uint8_t cmd, uint32_t arg) {
    if (cmd & 0x80) {
        cmd &= 0x7F;
        uint8_t r = send_cmd(CMD55, 0);
        if (r > 1)
            return r;
    }
    if (cmd != CMD12) {
        card_deselect();
        if (!card_select())
            return 0xFF;
    }
    uint8_t pkt[6] = {
        (uint8_t)(0x40 | cmd), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16),
        (uint8_t)(arg >> 8),   (uint8_t)arg,
        cmd == CMD0 ? 0x95 : cmd == CMD8 ? 0x87 : 0x01, // only these two are checked
    };
    spi_write_blocking(SD_SPI, pkt, sizeof pkt);
    if (cmd == CMD12)
        xchg(0xFF); // stuff byte
    uint8_t r;
    int n = 10;
    do
        r = xchg(0xFF);
    while ((r & 0x80) && --n);
    return r;
}

void sd_card_pins_init(void) {
    spi_init(SD_SPI, INIT_HZ);
    spi_set_format(SD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(SD_MISO_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SD_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SD_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_pull_up(SD_MISO_PIN);
    gpio_init(SD_CS_PIN);
    gpio_put(SD_CS_PIN, 1);
    gpio_set_dir(SD_CS_PIN, GPIO_OUT);
}

void sd_card_eject(void) { stat = STA_NOINIT; }

const char *sd_card_type(void) {
    if (stat & STA_NOINIT)
        return "none";
    if (card_type & CT_SDC2)
        return card_type & CT_BLOCK ? "SDHC/SDXC" : "SD v2";
    return card_type & CT_SDC1 ? "SD v1" : "MMC";
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv)
        return STA_NOINIT;
    spi_set_baudrate(SD_SPI, INIT_HZ);
    gpio_put(SD_CS_PIN, 1);
    for (int i = 0; i < 10; i++) // 80+ clocks with CS high: into SPI mode
        xchg(0xFF);

    uint8_t ty = 0, ocr[4];
    if (send_cmd(CMD0, 0) == 1) {
        absolute_time_t until = make_timeout_time_ms(1000);
        if (send_cmd(CMD8, 0x1AA) == 1) {
            spi_read_blocking(SD_SPI, 0xFF, ocr, 4);
            if (ocr[2] == 0x01 && ocr[3] == 0xAA) { // 2.7-3.6 V, echo ok
                while (!time_reached(until) && send_cmd(ACMD41, 1u << 30))
                    ;
                if (!time_reached(until) && send_cmd(CMD58, 0) == 0) {
                    spi_read_blocking(SD_SPI, 0xFF, ocr, 4);
                    ty = (ocr[0] & 0x40) ? CT_SDC2 | CT_BLOCK : CT_SDC2;
                }
            }
        } else {
            uint8_t cmd;
            if (send_cmd(ACMD41, 0) <= 1) {
                ty = CT_SDC1;
                cmd = ACMD41;
            } else {
                ty = CT_MMC;
                cmd = CMD1;
            }
            while (!time_reached(until) && send_cmd(cmd, 0))
                ;
            if (time_reached(until) || send_cmd(CMD16, 512) != 0)
                ty = 0;
        }
    }
    card_type = ty;
    card_deselect();
    if (ty) {
        spi_set_baudrate(SD_SPI, SD_SPI_HZ);
        stat = 0;
    } else {
        stat = STA_NOINIT;
    }
    return stat;
}

DSTATUS disk_status(BYTE pdrv) { return pdrv ? STA_NOINIT : stat; }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv || !count)
        return RES_PARERR;
    if (stat & STA_NOINIT)
        return RES_NOTRDY;
    if (!(card_type & CT_BLOCK))
        sector *= 512;
    if (count == 1) {
        if (send_cmd(CMD17, (uint32_t)sector) == 0 && rx_datablock(buff, 512))
            count = 0;
    } else if (send_cmd(CMD18, (uint32_t)sector) == 0) {
        do {
            if (!rx_datablock(buff, 512))
                break;
            buff += 512;
        } while (--count);
        send_cmd(CMD12, 0);
    }
    card_deselect();
    return count ? RES_ERROR : RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv || !count)
        return RES_PARERR;
    if (stat & STA_NOINIT)
        return RES_NOTRDY;
    if (!(card_type & CT_BLOCK))
        sector *= 512;
    if (count == 1) {
        if (send_cmd(CMD24, (uint32_t)sector) == 0 && tx_datablock(buff, 0xFE))
            count = 0;
    } else {
        if (card_type & (CT_SDC1 | CT_SDC2))
            send_cmd(ACMD23, count);
        if (send_cmd(CMD25, (uint32_t)sector) == 0) {
            do {
                if (!tx_datablock(buff, 0xFC))
                    break;
                buff += 512;
            } while (--count);
            if (!tx_datablock(NULL, 0xFD))
                count = 1;
        }
    }
    card_deselect();
    return count ? RES_ERROR : RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    (void)buff;
    if (pdrv)
        return RES_PARERR;
    if (stat & STA_NOINIT)
        return RES_NOTRDY;
    if (cmd == CTRL_SYNC) {
        bool ok = card_select();
        card_deselect();
        return ok ? RES_OK : RES_ERROR;
    }
    return RES_PARERR;
}

// File timestamps by the node's clock (UTC), once the link has heard one.
DWORD get_fattime(void) { return msglog_fattime(mesh_link_epoch()); }
