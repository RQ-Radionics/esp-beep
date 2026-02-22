/*
 * WD1770 FDC emulation for ESP32/ESP-IDF
 *
 * Derived from B-em (https://github.com/stardot/b-em) by Tom Walker.
 * Adapted for ESP-IDF: removed all B-em/Allegro/ddnoise/led dependencies.
 * Self-contained sector-level emulation for BBC Micro .ssd/.dsd images.
 *
 * Original code: GPL-2.0
 * This adaptation: GPL-2.0
 *
 * Key simplifications vs B-em:
 *   - No globals; all state in wd1770_t struct passed by pointer.
 *   - No B-em fdc_callback/fdc_data/fdc_spindown function pointer machinery;
 *     replaced by direct calls through the callbacks in wd1770_t.
 *   - Timing (fdc_time) replaced by fdc->delay_cycles decremented in tick().
 *   - Read/write operations complete synchronously into fdc->buf; the CPU
 *     reads bytes one at a time via the data register (DRQ-driven).
 *   - Track-level ops (read/write track, read address) are stubbed: they set
 *     NOT_FOUND so the OS falls back gracefully.
 *   - NMI is signalled via the irq() callback (BBC Micro routes INTRQ to NMI).
 */

#include <string.h>
#include "wd1770.h"

#ifdef ESP_PLATFORM
#  include "esp_log.h"
#  define WD_LOGD(fmt, ...) ESP_LOGD("wd1770", fmt, ##__VA_ARGS__)
#  define WD_LOGW(fmt, ...) ESP_LOGW("wd1770", fmt, ##__VA_ARGS__)
#else
#  include <stdio.h>
#  define WD_LOGD(fmt, ...) fprintf(stderr, "[wd] " fmt "\n", ##__VA_ARGS__)
#  define WD_LOGW(fmt, ...) fprintf(stderr, "[wd] WARN: " fmt "\n", ##__VA_ARGS__)
#endif

/* Delay constants (in 2 MHz BBC clock cycles, as in B-em) */
#define DELAY_CMD_START  32    /* time before first command callback */
#define DELAY_SEEK_STEP  6000  /* ~3ms per step at slowest rate */
#define DELAY_SECTOR_GAP 5000  /* inter-sector gap for multi-sector ops */
#define DELAY_COMPLETE   100   /* time before completion callback */
#define DELAY_FAULT      200   /* time before fault callback */
#define DELAY_ABORT      200   /* time for force-interrupt abort */
#define DELAY_SEEK_ALLOW 800   /* allow time for Opus DDOS seek cancel */

/* INDEX pulse timing.
 * 300 RPM = 5 rev/sec = 200 ms/rev = 400000 cycles/rev @ 2 MHz.
 * The INDEX pulse is active for ~2 ms = 4000 cycles. */
#define INDEX_PERIOD_CYCLES 400000
#define INDEX_PULSE_CYCLES    4000

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

static void set_intrq(wd1770_t *fdc, bool state)
{
    bool was = fdc->intrq;
    fdc->intrq = state;
    /* Only fire the callback on a rising edge (false→true transition).
     * This models the edge-triggered NMI on the BBC: once the /NMI line
     * has gone low (INTRQ asserted) and been serviced, it won't re-trigger
     * until INTRQ is first cleared (status register read) and then asserted
     * again.  Suppressing repeated asserts prevents the CPU from receiving
     * a storm of NMIs from the INDEX pulse train. */
    if (state && !was && fdc->cb.irq)
        fdc->cb.irq(fdc->cb.user_ctx, state);
    else if (!state && fdc->cb.irq)
        fdc->cb.irq(fdc->cb.user_ctx, state);  /* always propagate clear */
}

static void set_drq(wd1770_t *fdc, bool state)
{
    fdc->drq = state;
    fdc->status = state
        ? (fdc->status |  WD1770_STATUS_DRQ)
        : (fdc->status & ~WD1770_STATUS_DRQ);
    if (fdc->cb.drq)
        fdc->cb.drq(fdc->cb.user_ctx, state);
}

static void spinup(wd1770_t *fdc)
{
    fdc->motor_on = true;
    fdc->status |= WD1770_STATUS_MOTOR_ON;
}

static void spindown(wd1770_t *fdc) __attribute__((unused));
static void spindown(wd1770_t *fdc)
{
    fdc->motor_on = false;
    fdc->status &= ~WD1770_STATUS_MOTOR_ON;
}

static void completed(wd1770_t *fdc)
{
    fdc->status &= ~WD1770_STATUS_BUSY;
    fdc->delay_cycles = 0;
    set_intrq(fdc, true);
    WD_LOGD("completed, status=%02X", fdc->status);
}

static void fault(wd1770_t *fdc, uint8_t flags, const char *desc)
{
    WD_LOGW("%s", desc); (void)desc;
    fdc->status |= flags;
    fdc->status &= ~WD1770_STATUS_BUSY;
    fdc->delay_cycles = 0;
    set_intrq(fdc, true);
}

/* -------------------------------------------------------------------------
 * Type I (seek/step) helpers
 * ------------------------------------------------------------------------- */

static void seek_done(wd1770_t *fdc, uint8_t cmd)
{
    /* Verify flag (bit 2): check track register matches disk track */
    if ((cmd & 0x04) && fdc->cur_drive < 2) {
        /* For sector-level emulation we trust the track register */
        WD_LOGD("seek done, track=%d", fdc->track);
    }
    if (fdc->cb.seek)
        fdc->cb.seek(fdc->cb.user_ctx, fdc->cur_drive, fdc->track);
    completed(fdc);
}

/* -------------------------------------------------------------------------
 * Type II (read/write sector) helpers
 * ------------------------------------------------------------------------- */

static void begin_read_sector(wd1770_t *fdc)
{
    WD_LOGD("read sector drive=%d side=%d track=%d sector=%d dens=%d",
            fdc->cur_drive, fdc->cur_side, fdc->track, fdc->sector, fdc->density);

    fdc->status = WD1770_STATUS_MOTOR_ON | WD1770_STATUS_BUSY;
    fdc->type1_status = false;

    if (!fdc->cb.read_sector) {
        fault(fdc, WD1770_STATUS_RNF, "read_sector callback not set");
        return;
    }

    fdc->buf_count = 0;
    fdc->buf_pos   = 0;
    int rc = fdc->cb.read_sector(fdc->cb.user_ctx,
                                 fdc->cur_drive, fdc->track, fdc->sector,
                                 fdc->cur_side, fdc->density,
                                 fdc->buf, &fdc->buf_count);
    if (rc != 0 || fdc->buf_count == 0) {
        fault(fdc, WD1770_STATUS_RNF, "sector not found");
        return;
    }

    /* First byte into data register, assert DRQ */
    fdc->data = fdc->buf[fdc->buf_pos++];
    set_drq(fdc, true);
    /* Schedule completion after all bytes transferred */
    fdc->delay_cycles = DELAY_COMPLETE;
}

static void begin_write_sector(wd1770_t *fdc)
{
    WD_LOGD("write sector drive=%d side=%d track=%d sector=%d dens=%d",
            fdc->cur_drive, fdc->cur_side, fdc->track, fdc->sector, fdc->density);

    if (fdc->write_protect) {
        fault(fdc, WD1770_STATUS_WRITE_PROT, "write protect");
        return;
    }
    if (!fdc->cb.write_sector) {
        fault(fdc, WD1770_STATUS_RNF, "write_sector callback not set");
        return;
    }

    fdc->status  = WD1770_STATUS_MOTOR_ON | WD1770_STATUS_DRQ | WD1770_STATUS_BUSY;
    fdc->type1_status = false;
    fdc->buf_pos   = 0;
    fdc->buf_count = 256; /* default DFS sector size; host may override */
    set_drq(fdc, true);
    fdc->delay_cycles = DELAY_COMPLETE;
}

static void finish_write_sector(wd1770_t *fdc)
{
    bool deleted = (fdc->command & 0x01) != 0;
    int rc = fdc->cb.write_sector(fdc->cb.user_ctx,
                                  fdc->cur_drive, fdc->track, fdc->sector,
                                  fdc->cur_side, fdc->density, deleted,
                                  fdc->buf, fdc->buf_pos);
    if (rc != 0)
        fault(fdc, WD1770_STATUS_RNF, "write sector failed");
    else
        completed(fdc);
}

/* -------------------------------------------------------------------------
 * Command state machine  (mirrors B-em wd1770_cmd_start / wd1770_cmd_next)
 * ------------------------------------------------------------------------- */

static void cmd_start(wd1770_t *fdc)
{
    uint8_t cmd = fdc->command;
    WD_LOGD("cmd_start op=%X cmd=%02X", cmd >> 4, cmd);

    switch (cmd >> 4) {
        case 0x0: /* Restore */
            fdc->status = WD1770_STATUS_MOTOR_ON | WD1770_STATUS_BUSY;
            fdc->type1_status = true;
            fdc->track     = 0xFF;
            fdc->data      = 0;
            fdc->seek_delta = (int8_t)(0 - (int)fdc->track);
            /* Step to track 0 – for sector-level emulation just snap */
            fdc->track = 0;
            fdc->delay_cycles = DELAY_SEEK_STEP;
            break;

        case 0x1: /* Seek */
            fdc->type1_status = true;
            fdc->seek_ok = false;
            fdc->delay_cycles = DELAY_SEEK_ALLOW;
            break;

        case 0x2: /* Step (no update) */
        case 0x3: /* Step (with update) */
            fdc->type1_status = true;
            if (cmd & 0x10) fdc->track += fdc->step_dir; /* update flag */
            fdc->delay_cycles = DELAY_SEEK_STEP;
            break;

        case 0x4: /* Step in (no update) */
        case 0x5: /* Step in (with update) */
            fdc->step_dir = 1;
            fdc->type1_status = true;
            if (cmd & 0x10) fdc->track++;
            fdc->delay_cycles = DELAY_SEEK_STEP;
            break;

        case 0x6: /* Step out (no update) */
        case 0x7: /* Step out (with update) */
            fdc->step_dir = -1;
            fdc->type1_status = true;
            if (cmd & 0x10) { if (fdc->track > 0) fdc->track--; }
            fdc->delay_cycles = DELAY_SEEK_STEP;
            break;

        case 0x8: /* Read single sector */
        case 0x9: /* Read multiple sectors */
            fdc->in_gap = false;
            begin_read_sector(fdc);
            break;

        case 0xA: /* Write single sector */
        case 0xB: /* Write multiple sectors */
            fdc->in_gap = false;
            begin_write_sector(fdc);
            break;

        case 0xC: /* Read address */
            WD_LOGD("read address (stub) side=%d track=%d", fdc->cur_side, fdc->track);
            fdc->status = WD1770_STATUS_MOTOR_ON | WD1770_STATUS_BUSY;
            fdc->type1_status = false;
            /* Stub: not supported in sector-level emulation */
            fdc->delay_cycles = DELAY_FAULT;
            break;

        case 0xD: /* Force interrupt */
            fdc->delay_cycles = DELAY_ABORT;
            break;

        case 0xE: /* Read track */
            WD_LOGD("read track (stub) side=%d track=%d", fdc->cur_side, fdc->track);
            fdc->status = WD1770_STATUS_MOTOR_ON | WD1770_STATUS_BUSY;
            fdc->type1_status = false;
            fdc->delay_cycles = DELAY_FAULT;
            break;

        case 0xF: /* Write track */
            WD_LOGD("write track (stub) side=%d track=%d", fdc->cur_side, fdc->track);
            fdc->status = WD1770_STATUS_MOTOR_ON | WD1770_STATUS_BUSY;
            fdc->type1_status = false;
            fdc->delay_cycles = DELAY_FAULT;
            break;
    }
    fdc->cmd_started = true;
}

static void cmd_next(wd1770_t *fdc)
{
    uint8_t cmd = fdc->command;
    WD_LOGD("cmd_next op=%X cmd=%02X", cmd >> 4, cmd);

    switch (cmd >> 4) {
        case 0x0: /* Restore complete */
            fdc->track = 0;
            fdc->seek_delta = 0;
            seek_done(fdc, cmd);
            break;

        case 0x1: /* Seek */
            if (!fdc->seek_ok) {
                /* Data register not written – ignore */
                fdc->status &= ~WD1770_STATUS_BUSY;
                WD_LOGW("seek ignored: data register not written");
            } else {
                fdc->seek_delta = (int8_t)((int)fdc->data - (int)fdc->track);
                fdc->track = fdc->data;
                fdc->delay_cycles = DELAY_SEEK_STEP;
                fdc->cmd_started = false; /* go through cmd_start path again */
                /* Reuse cmd_next for second phase via a synthetic start */
                seek_done(fdc, cmd);
            }
            break;

        case 0x2: /* Step (no update) complete */
        case 0x3: /* Step (with update) complete */
        case 0x4: /* Step in (no update) complete */
        case 0x5: /* Step in (with update) complete */
        case 0x6: /* Step out (no update) complete */
        case 0x7: /* Step out (with update) complete */
            seek_done(fdc, cmd);
            break;

        case 0x8: /* Read single sector complete */
            /* Only complete once all bytes have been consumed by the CPU.
             * If bytes remain the CPU has not finished reading via DRQ/NMI;
             * reschedule so we poll again shortly. */
            if (fdc->buf_pos < fdc->buf_count) {
                fdc->delay_cycles = DELAY_COMPLETE;
            } else {
                completed(fdc);
            }
            break;

        case 0x9: /* Read multiple sectors */
            if (fdc->status & (WD1770_STATUS_WRITE_PROT |
                               WD1770_STATUS_RNF |
                               WD1770_STATUS_CRC_ERROR)) {
                completed(fdc);
            } else if (fdc->in_gap) {
                fdc->sector++;
                fdc->in_gap = false;
                begin_read_sector(fdc);
            } else {
                fdc->in_gap = true;
                fdc->delay_cycles = DELAY_SECTOR_GAP;
            }
            break;

        case 0xA: /* Write single sector complete */
            finish_write_sector(fdc);
            break;

        case 0xB: /* Write multiple sectors */
            if (fdc->status & (WD1770_STATUS_WRITE_PROT |
                               WD1770_STATUS_RNF |
                               WD1770_STATUS_CRC_ERROR)) {
                finish_write_sector(fdc);
            } else if (fdc->in_gap) {
                finish_write_sector(fdc);
                fdc->sector++;
                fdc->in_gap = false;
                begin_write_sector(fdc);
            } else {
                fdc->in_gap = true;
                fdc->delay_cycles = DELAY_SECTOR_GAP;
            }
            break;

        case 0xC: /* Read address (stub) */
            fault(fdc, WD1770_STATUS_RNF, "read address not supported");
            break;

        case 0xD: /* Force interrupt */
            if (fdc->status & WD1770_STATUS_BUSY)
                fdc->status &= ~WD1770_STATUS_BUSY;
            else
                fdc->status = WD1770_STATUS_MOTOR_ON;
            set_intrq(fdc, true);
            fdc->seek_ok = false;
            WD_LOGD("force interrupt done");
            break;

        case 0xE: /* Read track (stub) */
        case 0xF: /* Write track (stub) */
            fault(fdc, WD1770_STATUS_RNF, "track-level op not supported");
            break;
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void wd1770_init(wd1770_t *fdc, const wd1770_callbacks_t *callbacks)
{
    memset(fdc, 0, sizeof(*fdc));
    if (callbacks)
        fdc->cb = *callbacks;
    fdc->step_dir = 1;
    wd1770_reset(fdc);
}

void wd1770_reset(wd1770_t *fdc)
{
    fdc->status      = 0;
    fdc->sector      = 1;
    fdc->data        = 0;
    fdc->command     = 0;
    fdc->cmd_started = false;
    fdc->type1_status = true;
    fdc->seek_ok     = false;
    fdc->in_gap      = false;
    fdc->drq         = false;
    fdc->intrq       = false;
    fdc->delay_cycles = 0;
    fdc->buf_pos     = 0;
    fdc->buf_count   = 0;

    /* Start with index pulse already active so the DFS hardware-detect
     * read of status (AND #$03) sees INDEX (bit 1) = 1 immediately.
     * The pulse will drop after INDEX_PULSE_CYCLES and repeat every
     * INDEX_PERIOD_CYCLES thereafter. */
    fdc->index_pulse  = true;
    fdc->index_cycles = INDEX_PULSE_CYCLES;

    if (fdc->motor_on)
        fdc->status |= WD1770_STATUS_MOTOR_ON;

    WD_LOGD("reset index_pulse=%d", fdc->index_pulse);
}

uint8_t wd1770_read(wd1770_t *fdc, uint8_t reg)
{
    switch (reg & 0x03) {
        case 0: { /* Status register – reading clears INTRQ */
            set_intrq(fdc, false);
            uint8_t s = fdc->status;
            if (fdc->type1_status) {
                if (fdc->track == 0)
                    s |= WD1770_STATUS_TRACK0;
                if (fdc->motor_on)
                    s |= WD1770_STATUS_SPIN_UP;
                /* INDEX pulse: bit 1 toggles once per disk revolution.
                 * The DFS 1770 ROM checks (status & 0x03) != 0 to confirm
                 * a disk is spinning.  We assert INDEX whenever not busy,
                 * which satisfies the DFS detect without needing exact
                 * revolution timing.  When busy, use the toggling pulse. */
                if (fdc->index_pulse || !(fdc->status & WD1770_STATUS_BUSY))
                    s |= WD1770_STATUS_INDEX;
            }
            WD_LOGD("read status base=%02X type1=%d idx=%d motor=%d -> %02X",
                    fdc->status, fdc->type1_status, fdc->index_pulse, fdc->motor_on, s);
            return s;
        }
        case 1:
            WD_LOGD("read track -> %02X", fdc->track);
            return fdc->track;
        case 2:
            WD_LOGD("read sector -> %02X", fdc->sector);
            return fdc->sector;
        case 3: { /* Data register – reading clears DRQ, loads next byte */
            set_drq(fdc, false);
            uint8_t d = fdc->data;
            if (fdc->buf_pos < fdc->buf_count) {
                fdc->data = fdc->buf[fdc->buf_pos++];
                if (fdc->buf_pos < fdc->buf_count)
                    set_drq(fdc, true);
                /* else last byte: DRQ stays clear, completion fires on next tick */
            }
            WD_LOGD("read data -> %02X (buf_pos=%d/%d)", d, fdc->buf_pos, fdc->buf_count);
            return d;
        }
    }
    return 0xFE;
}

void wd1770_write(wd1770_t *fdc, uint8_t reg, uint8_t val)
{
    switch (reg & 0x03) {
        case 0: /* Command register */
            if ((val & 0xF0) != 0xD0) { /* Force interrupt bypasses busy check */
                if (fdc->status & WD1770_STATUS_BUSY) {
                    WD_LOGW("cmd %02X rejected: device busy", val);
                    return;
                }
                fdc->status |= WD1770_STATUS_BUSY;
                spinup(fdc);
            }
            set_intrq(fdc, false);
            fdc->cmd_started  = false;
            fdc->command      = val;
            fdc->delay_cycles = DELAY_CMD_START;
            WD_LOGD("write command %02X", val);
            break;

        case 1: /* Track register */
            if (fdc->status & WD1770_STATUS_BUSY) {
                WD_LOGW("track write %02X rejected: busy", val);
                return;
            }
            fdc->track = val;
            WD_LOGD("write track %02X", val);
            break;

        case 2: /* Sector register */
            if (fdc->status & WD1770_STATUS_BUSY) {
                WD_LOGW("sector write %02X rejected: busy", val);
                return;
            }
            fdc->sector = val;
            WD_LOGD("write sector %02X", val);
            break;

        case 3: /* Data register */
            set_drq(fdc, false);
            fdc->data    = val;
            fdc->seek_ok = true;
            /* For write sector: accumulate in buffer */
            if ((fdc->status & WD1770_STATUS_BUSY) &&
                ((fdc->command & 0xE0) == 0xA0)) { /* write sector commands */
                if (fdc->buf_pos < sizeof(fdc->buf))
                    fdc->buf[fdc->buf_pos++] = val;
                /* Request next byte if more expected */
                if (fdc->buf_pos < fdc->buf_count)
                    set_drq(fdc, true);
            }
            WD_LOGD("write data %02X", val);
            break;
    }
}

void wd1770_tick(wd1770_t *fdc, int32_t cycles)
{
    /* --- INDEX pulse generator ---
     * Toggle the index pulse once per revolution.  The pulse stays high for
     * INDEX_PULSE_CYCLES then low for (INDEX_PERIOD_CYCLES - INDEX_PULSE_CYCLES).
     * This is independent of any command in progress.
     *
     * When the chip is idle (not BUSY) and the motor is on, the WD1770
     * asserts INTRQ on the rising edge of each INDEX pulse.  The BBC DFS
     * relies on this to wake up from its "wait for idle" spin loop ($8E85)
     * after a seek/step command completes. */
    /* Use a while loop so that large cycle counts don't skip transitions. */
    fdc->index_cycles -= cycles;
    while (fdc->index_cycles <= 0) {
        bool was_pulse = fdc->index_pulse;
        fdc->index_pulse = !fdc->index_pulse;
        fdc->index_cycles += fdc->index_pulse
            ? INDEX_PULSE_CYCLES
            : (INDEX_PERIOD_CYCLES - INDEX_PULSE_CYCLES);
        /* Rising edge (LOW→HIGH) while idle → assert INTRQ once.
         * Guard against re-asserting if INTRQ is already pending (i.e. the
         * previous NMI handler hasn't cleared it yet by reading the status
         * register).  Without this guard the while-loop above can generate
         * multiple rising edges in a single tick and flood the CPU with NMIs. */
        if (!was_pulse && fdc->index_pulse &&
            fdc->motor_on && !(fdc->status & WD1770_STATUS_BUSY) &&
            !fdc->intrq) {
            WD_LOGD("INDEX rising edge while idle → INTRQ");
            set_intrq(fdc, true);
        }
    }

    /* --- Command state machine --- */
    if (fdc->delay_cycles <= 0)
        return;

    fdc->delay_cycles -= cycles;
    if (fdc->delay_cycles > 0)
        return;

    fdc->delay_cycles = 0;

    if (!fdc->cmd_started)
        cmd_start(fdc);
    else
        cmd_next(fdc);
}

void wd1770_select(wd1770_t *fdc, uint8_t drive, uint8_t side, uint8_t density)
{
    WD_LOGD("select drive=%d side=%d density=%d", drive, side, density);
    fdc->cur_drive = drive & 0x01;
    fdc->cur_side  = side  & 0x01;
    fdc->density   = density & 0x01;
}

bool wd1770_get_drq(const wd1770_t *fdc)
{
    return fdc->drq;
}

bool wd1770_get_intrq(const wd1770_t *fdc)
{
    return fdc->intrq;
}

bool wd1770_get_motor(const wd1770_t *fdc)
{
    return fdc->motor_on;
}
