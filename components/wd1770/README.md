# wd1770 — WD1770 FDC component for ESP32/ESP-IDF

Sector-level emulation of the Western Digital WD1770 Floppy Disk Controller,
adapted from [B-em](https://github.com/stardot/b-em) for use as a self-contained
ESP-IDF component targeting BBC Micro `.ssd` and `.dsd` disk images.

## Origin and licence

Derived from B-em by Tom Walker (GPL-2.0).  This adaptation is also GPL-2.0.

## What is emulated

- All Type I commands: Restore, Seek, Step, Step In, Step Out
- Type II commands: Read Sector (single and multiple), Write Sector (single and multiple)
- Type III stubs: Read Address, Read Track, Write Track return RNF so the OS falls back gracefully
- Force Interrupt (0xD0)
- Status register with correct bit semantics for Type I vs Type II/III
- INTRQ and DRQ signals via callbacks or polling
- Motor-on status bit

## What is NOT emulated

- Track-level MFM/FM encoding (not needed for sector-level SSD/DSD)
- CRC generation/checking (sectors are assumed correct from the image file)
- Index pulse timing
- Write track / format (stubs only)
- The drive-select, side-select and density-select registers are **external** to
  the WD1770 on the BBC Micro — the host must call `wd1770_select()` when the
  System VIA port B changes.

## BBC Micro register map

The WD1770 is accessed at `&FE84`–`&FE87` on the BBC B/B+ (Acorn interface):

| Address | Read          | Write         |
|---------|---------------|---------------|
| &FE84   | Status        | Command       |
| &FE85   | Track         | Track         |
| &FE86   | Sector        | Sector        |
| &FE87   | Data          | Data          |

The control latch at `&FE80` (drive, side, density, reset) is handled by the
host; call `wd1770_select()` and `wd1770_reset()` accordingly.

On the BBC Master the registers are at `&FE28`–`&FE2B` and `&FE24` for control.

## API

```c
#include "wd1770.h"

/* 1. Fill in callbacks */
wd1770_callbacks_t cb = {
    .read_sector  = my_read_sector,
    .write_sector = my_write_sector,
    .seek         = NULL,
    .irq          = my_irq_handler,   /* routes to 6502 NMI */
    .drq          = NULL,             /* use polling or IRQ */
    .user_ctx     = my_disk_array,
};

/* 2. Initialise */
wd1770_t fdc;
wd1770_init(&fdc, &cb);

/* 3. Select drive/side/density (called from VIA write handler) */
wd1770_select(&fdc, drive, side, WD1770_DENSITY_MFM);

/* 4. Register access (called from 6502 memory map) */
uint8_t val = wd1770_read(&fdc, addr & 0x03);
wd1770_write(&fdc, addr & 0x03, val);

/* 5. Clock tick (called every BBC 2 MHz cycle, or batch) */
wd1770_tick(&fdc, cycles);
```

See `examples/ssd_disk.c` for a complete wiring example with `.ssd`/`.dsd`
image files via ESP-IDF VFS.

## Disk image format

| Format | Sides | Tracks | Sectors/track | Bytes/sector | Total   |
|--------|-------|--------|---------------|--------------|---------|
| `.ssd` | 1     | 80     | 10            | 256          | 200 KB  |
| `.dsd` | 2     | 80     | 10            | 256          | 400 KB  |

`.dsd` layout: sides are interleaved by track
(`track0/side0`, `track0/side1`, `track1/side0`, …).

## References

- [WD1770 datasheet](https://datasheetspdf.com/pdf/549289/WDC/WD1770/1)
- [B-em source](https://github.com/stardot/b-em/blob/master/src/wd1770.c)
- [BBC Micro DFS technical reference](https://beebwiki.mdfs.net/Acorn_DFS)
- [Stardot forums — WD1770 timing](https://stardot.org.uk/forums)
