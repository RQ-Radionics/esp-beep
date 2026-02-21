/*
 * ssd_disk.c — WD1770 callbacks for BBC Micro .ssd and .dsd disk images
 *
 * Implements the wd1770_callbacks_t interface for sector-level access to
 * flat disk image files mounted via ESP-IDF VFS (e.g. from SD card or SPIFFS).
 *
 * Disk geometry (DFS, single/double density):
 *   .ssd  Single-sided:  80 tracks × 10 sectors × 256 bytes = 200 KB
 *   .dsd  Double-sided:  80 tracks × 10 sectors × 256 bytes × 2 = 400 KB
 *         Layout: interleaved by side (track 0 side 0, track 0 side 1, ...)
 *
 * Usage:
 *   ssd_disk_t disk0, disk1;
 *   ssd_disk_open(&disk0, "/sdcard/elite.ssd", false);
 *
 *   wd1770_callbacks_t cb = {
 *       .read_sector  = ssd_read_sector,
 *       .write_sector = ssd_write_sector,
 *       .seek         = NULL,
 *       .irq          = my_irq_handler,
 *       .drq          = NULL,
 *       .user_ctx     = &disk0,  // or array of disks, indexed by drive
 *   };
 *   wd1770_init(&fdc, &cb);
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "wd1770.h"

/* -------------------------------------------------------------------------
 * Disk image descriptor
 * ------------------------------------------------------------------------- */

#define SSD_TRACKS        80
#define SSD_SECTORS       10
#define SSD_SECTOR_SIZE   256
#define SSD_SIDES_SINGLE  1
#define SSD_SIDES_DOUBLE  2

typedef struct {
    FILE    *fp;
    bool     double_sided;   /* true = .dsd, false = .ssd */
    bool     write_protect;
    uint8_t  num_tracks;     /* usually 80, sometimes 40 */
} ssd_disk_t;

/*
 * Open a disk image file.
 * double_sided: true for .dsd, false for .ssd
 * Returns 0 on success, -1 on error.
 */
int ssd_disk_open(ssd_disk_t *disk, const char *path, bool double_sided)
{
    memset(disk, 0, sizeof(*disk));
    disk->fp = fopen(path, "r+b");
    if (!disk->fp) {
        /* Try read-only */
        disk->fp = fopen(path, "rb");
        if (!disk->fp)
            return -1;
        disk->write_protect = true;
    }
    disk->double_sided = double_sided;
    disk->num_tracks   = SSD_TRACKS;
    return 0;
}

void ssd_disk_close(ssd_disk_t *disk)
{
    if (disk->fp) {
        fclose(disk->fp);
        disk->fp = NULL;
    }
}

/* -------------------------------------------------------------------------
 * Offset calculation
 *
 * .ssd: linear by track then sector
 *   offset = (track * 10 + sector) * 256
 *
 * .dsd: interleaved sides
 *   offset = ((track * 2 + side) * 10 + sector) * 256
 * ------------------------------------------------------------------------- */

static long sector_offset(const ssd_disk_t *disk,
                           uint8_t track, uint8_t sector, uint8_t side)
{
    if (sector < 0 || sector >= SSD_SECTORS)
        return -1;
    if (track >= disk->num_tracks)
        return -1;

    long offset;
    if (disk->double_sided)
        offset = ((long)(track * 2 + (side & 1)) * SSD_SECTORS + sector)
                 * SSD_SECTOR_SIZE;
    else
        offset = ((long)track * SSD_SECTORS + sector) * SSD_SECTOR_SIZE;

    return offset;
}

/* -------------------------------------------------------------------------
 * Callback: read one sector
 * user_ctx is a pointer to ssd_disk_t (single drive) or ssd_disk_t[2] (two drives)
 * ------------------------------------------------------------------------- */
int ssd_read_sector(void *user_ctx,
                    uint8_t drive, uint8_t track, uint8_t sector,
                    uint8_t side, uint8_t density,
                    uint8_t *buf, uint16_t *len)
{
    (void)density; /* DFS ignores density for offset calculation */

    ssd_disk_t *disks = (ssd_disk_t *)user_ctx;
    ssd_disk_t *disk  = &disks[drive & 0x01];

    if (!disk->fp)
        return -1;

    long offset = sector_offset(disk, track, sector, side);
    if (offset < 0)
        return -1;

    if (fseek(disk->fp, offset, SEEK_SET) != 0)
        return -1;

    size_t n = fread(buf, 1, SSD_SECTOR_SIZE, disk->fp);
    *len = (uint16_t)n;
    return (n == SSD_SECTOR_SIZE) ? 0 : -1;
}

/* -------------------------------------------------------------------------
 * Callback: write one sector
 * ------------------------------------------------------------------------- */
int ssd_write_sector(void *user_ctx,
                     uint8_t drive, uint8_t track, uint8_t sector,
                     uint8_t side, uint8_t density, bool deleted,
                     const uint8_t *buf, uint16_t len)
{
    (void)density;
    (void)deleted; /* flat images don't store deleted-data marks */

    ssd_disk_t *disks = (ssd_disk_t *)user_ctx;
    ssd_disk_t *disk  = &disks[drive & 0x01];

    if (!disk->fp || disk->write_protect)
        return -1;

    if (len < SSD_SECTOR_SIZE)
        return -1;

    long offset = sector_offset(disk, track, sector, side);
    if (offset < 0)
        return -1;

    if (fseek(disk->fp, offset, SEEK_SET) != 0)
        return -1;

    size_t n = fwrite(buf, 1, SSD_SECTOR_SIZE, disk->fp);
    fflush(disk->fp);
    return (n == SSD_SECTOR_SIZE) ? 0 : -1;
}

/* -------------------------------------------------------------------------
 * Example: wiring it all together
 *
 * Uncomment and adapt for your BBC Micro emulator main loop.
 * ------------------------------------------------------------------------- */

#if 0

#include "wd1770.h"

static wd1770_t   fdc;
static ssd_disk_t disks[2];   /* drive 0 and drive 1 */

static void my_irq(void *ctx, bool state)
{
    (void)ctx;
    /* On BBC Micro, INTRQ is routed to NMI via the 1770 interface board */
    if (state)
        bbc_nmi_assert();   /* trigger NMI on the 6502 */
}

void bbc_fdc_init(void)
{
    ssd_disk_open(&disks[0], "/sdcard/elite.ssd", false);
    /* disks[1] left empty (no disk) */

    wd1770_callbacks_t cb = {
        .read_sector  = ssd_read_sector,
        .write_sector = ssd_write_sector,
        .seek         = NULL,
        .irq          = my_irq,
        .drq          = NULL,
        .user_ctx     = disks,
    };
    wd1770_init(&fdc, &cb);

    /* BBC Micro: drive 0, side 0, double-density (MFM) */
    wd1770_select(&fdc, 0, 0, WD1770_DENSITY_MFM);
}

/* Call from the BBC Micro VIA write handler for the control register */
void bbc_fdc_control_write(uint8_t val)
{
    /* Acorn-style: bit1=drive, bit2=side, bit3=density(inv), bit5=reset(inv) */
    uint8_t drive   = (val & 0x02) ? 1 : 0;
    uint8_t side    = (val & 0x04) ? 1 : 0;
    uint8_t density = (val & 0x08) ? WD1770_DENSITY_FM : WD1770_DENSITY_MFM;
    wd1770_select(&fdc, drive, side, density);

    if (!(val & 0x20))          /* active-low reset */
        wd1770_reset(&fdc);
}

/* Call from 6502 memory read at &FE84-&FE87 */
uint8_t bbc_fdc_read(uint16_t addr)
{
    return wd1770_read(&fdc, (uint8_t)(addr & 0x03));
}

/* Call from 6502 memory write at &FE84-&FE87 */
void bbc_fdc_write(uint16_t addr, uint8_t val)
{
    wd1770_write(&fdc, (uint8_t)(addr & 0x03), val);
}

/* Call every 2 MHz BBC clock tick (or pass accumulated cycles) */
void bbc_fdc_tick(int cycles)
{
    wd1770_tick(&fdc, cycles);
}

#endif /* 0 */
