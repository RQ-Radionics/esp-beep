/*
 * WD1770 FDC emulation for ESP32/ESP-IDF
 *
 * Derived from B-em (https://github.com/stardot/b-em) by Tom Walker.
 * Adapted for ESP-IDF: removed all B-em/Allegro dependencies, converted to
 * a self-contained struct-based API suitable for sector-level emulation of
 * BBC Micro .ssd and .dsd disk images.
 *
 * Original code: GPL-2.0
 * This adaptation: GPL-2.0
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Status register bit definitions
 * Bits have different meanings for Type I (seek/step) vs Type II/III commands
 * -------------------------------------------------------------------------- */
#define WD1770_STATUS_MOTOR_ON      0x80
#define WD1770_STATUS_WRITE_PROT    0x40
#define WD1770_STATUS_SPIN_UP       0x20  /* Type I: motor spun up */
#define WD1770_STATUS_DELETED_DATA  0x20  /* Type II/III: deleted data mark */
#define WD1770_STATUS_SEEK_ERROR    0x10  /* Type I */
#define WD1770_STATUS_RNF           0x10  /* Type II/III: record not found */
#define WD1770_STATUS_CRC_ERROR     0x08
#define WD1770_STATUS_TRACK0        0x04  /* Type I: head at track 0 */
#define WD1770_STATUS_LOST_DATA     0x04  /* Type II/III */
#define WD1770_STATUS_DRQ           0x02  /* Type II/III: data request */
#define WD1770_STATUS_INDEX         0x02  /* Type I: index pulse */
#define WD1770_STATUS_BUSY          0x01

/* --------------------------------------------------------------------------
 * Density flags (match B-em DISC_FLAG_* for compatibility)
 * -------------------------------------------------------------------------- */
#define WD1770_DENSITY_FM   0x00  /* Single density (FM) */
#define WD1770_DENSITY_MFM  0x01  /* Double density (MFM) */
#define WD1770_FLAG_DELETED 0x02  /* Deleted data mark */

/* --------------------------------------------------------------------------
 * Callbacks
 *
 * The BBC Micro's drive/side/density selection is done externally (via the
 * System VIA), not by the WD1770 itself. The host code calls wd1770_select()
 * and provides these callbacks for actual I/O.
 * -------------------------------------------------------------------------- */
typedef struct {
    /*
     * Read one sector. Returns 0 on success, -1 on error (RNF).
     * buf must be at least 1024 bytes. *len is set to bytes read.
     * For DFS: sector size is always 256 bytes.
     */
    int (*read_sector)(void *user_ctx,
                       uint8_t drive, uint8_t track, uint8_t sector,
                       uint8_t side, uint8_t density,
                       uint8_t *buf, uint16_t *len);

    /*
     * Write one sector. Returns 0 on success, -1 on error.
     * deleted=true when writing a deleted-data-mark sector.
     */
    int (*write_sector)(void *user_ctx,
                        uint8_t drive, uint8_t track, uint8_t sector,
                        uint8_t side, uint8_t density, bool deleted,
                        const uint8_t *buf, uint16_t len);

    /*
     * Physical seek notification. May be NULL.
     * Called after any seek/step that moves the head.
     */
    void (*seek)(void *user_ctx, uint8_t drive, uint8_t track);

    /*
     * INTRQ pin state change. May be NULL (use wd1770_get_intrq() for polling).
     * state=true means interrupt asserted.
     */
    void (*irq)(void *user_ctx, bool state);

    /*
     * DRQ pin state change. May be NULL (use wd1770_get_drq() for polling).
     * state=true means data request asserted.
     */
    void (*drq)(void *user_ctx, bool state);

    /* Opaque context pointer passed to all callbacks */
    void *user_ctx;
} wd1770_callbacks_t;

/* --------------------------------------------------------------------------
 * FDC state
 *
 * All state is in this struct; no globals. Allocate statically or on heap.
 * -------------------------------------------------------------------------- */
typedef struct {
    /* Visible registers */
    uint8_t  status;
    uint8_t  track;
    uint8_t  sector;
    uint8_t  data;

    /* Internal command state */
    uint8_t  command;
    int8_t   step_dir;       /* +1 = inward (higher track), -1 = outward */
    uint8_t  cur_drive;      /* 0 or 1 */
    uint8_t  cur_side;       /* 0 or 1 */
    uint8_t  density;        /* WD1770_DENSITY_FM or WD1770_DENSITY_MFM */
    bool     motor_on;
    bool     drq;
    bool     intrq;
    bool     write_protect;
    bool     type1_status;   /* true = Type I status interpretation */
    bool     cmd_started;    /* false = start phase, true = next/complete phase */
    bool     in_gap;         /* multi-sector: in inter-sector gap */
    bool     seek_ok;        /* seek: data register has been written */
    int8_t   seek_delta;     /* seek: tracks to move */

    /* Sector transfer buffer */
    uint8_t  buf[1024];
    uint16_t buf_pos;
    uint16_t buf_count;

    /* Simplified timing: counts down in wd1770_tick(), triggers callback at 0 */
    int32_t  delay_cycles;

    /* INDEX pulse counter.
     * The WD1770 receives one INDEX pulse per disk revolution (~300 RPM →
     * 200 ms period).  At 2 MHz this is 400000 cycles/revolution.
     * The INDEX bit (bit 1) in Type-I status is set for a short pulse (~2 ms
     * = 4000 cycles) once per revolution.  The DFS uses this to confirm a
     * disk is spinning before issuing commands. */
    int32_t  index_cycles;   /* cycles until next index pulse edge */
    bool     index_pulse;    /* current index pulse state */

    /* Callbacks */
    wd1770_callbacks_t cb;
} wd1770_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/* Initialise FDC with the provided I/O callbacks */
void wd1770_init(wd1770_t *fdc, const wd1770_callbacks_t *callbacks);

/* Hardware reset (MR pin asserted) */
void wd1770_reset(wd1770_t *fdc);

/*
 * Register access (reg 0-3).
 *   Read:  0=status, 1=track, 2=sector, 3=data
 *   Write: 0=command, 1=track, 2=sector, 3=data
 */
uint8_t wd1770_read(wd1770_t *fdc, uint8_t reg);
void    wd1770_write(wd1770_t *fdc, uint8_t reg, uint8_t val);

/*
 * Clock tick.
 * Call once per CPU instruction cycle batch (or once per BBC Micro 2 MHz
 * clock tick). For sector-level emulation exact timing is not critical;
 * passing cycles=1 each call is sufficient.
 */
void wd1770_tick(wd1770_t *fdc, int32_t cycles);

/*
 * Drive/side/density selection.
 * In the BBC Micro this is controlled by the System VIA port B, not the
 * WD1770 itself. The host must call this whenever the VIA output changes.
 */
void wd1770_select(wd1770_t *fdc, uint8_t drive, uint8_t side, uint8_t density);

/* Signal polling (use when callbacks are NULL) */
bool wd1770_get_drq(const wd1770_t *fdc);
bool wd1770_get_intrq(const wd1770_t *fdc);
bool wd1770_get_motor(const wd1770_t *fdc);

#ifdef __cplusplus
}
#endif
