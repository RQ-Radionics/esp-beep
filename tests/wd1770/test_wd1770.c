/*
 * test_wd1770.c — Unit tests for the wd1770 FDC component
 *
 * Tests cover:
 *   - Reset: status, registers cleared
 *   - Restore: moves head to track 0, INTRQ fires
 *   - Seek: head moves to target track, INTRQ fires
 *   - Step In / Step Out: track register updated correctly
 *   - Read sector: DRQ fires, data delivered byte by byte, INTRQ on completion
 *   - Read sector: RNF when callback returns error
 *   - Write sector: DRQ fires, bytes accumulated, write callback invoked
 *   - Write protect: write sector rejected with WRITE_PROT status
 *   - Force interrupt: aborts busy command
 *   - Track / Sector / Data register reads and writes
 *   - Status register: Type I vs Type II bits
 *   - Read track / Read address: stubbed as RNF
 *   - Multi-sector read: increments sector, reads next
 *
 * Build and run:
 *   make -C tests/wd1770 && tests/wd1770/test_wd1770
 *
 * Licence: GPL-2.0
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#include "../../components/wd1770/include/wd1770.h"

/* -------------------------------------------------------------------------
 * Minimal test framework
 * ------------------------------------------------------------------------- */
static int s_tests_run    = 0;
static int s_tests_failed = 0;
static const char *s_current_test = "";

#define TEST(name) \
    do { \
        s_current_test = #name; \
        s_tests_run++; \
        test_##name(); \
    } while(0)

#define ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            fprintf(stderr, "  FAIL [%s] line %d: " #a " == %d, expected %d\n", \
                    s_current_test, __LINE__, (int)(a), (int)(b)); \
            s_tests_failed++; \
            return; \
        } \
    } while(0)

#define ASSERT_TRUE(x) \
    do { \
        if (!(x)) { \
            fprintf(stderr, "  FAIL [%s] line %d: " #x " is false\n", \
                    s_current_test, __LINE__); \
            s_tests_failed++; \
            return; \
        } \
    } while(0)

#define ASSERT_FALSE(x) \
    do { \
        if (x) { \
            fprintf(stderr, "  FAIL [%s] line %d: " #x " is true\n", \
                    s_current_test, __LINE__); \
            s_tests_failed++; \
            return; \
        } \
    } while(0)

static void test_pass(void) {
    printf("  PASS [%s]\n", s_current_test);
}

/* -------------------------------------------------------------------------
 * Callback tracking
 * ------------------------------------------------------------------------- */
static bool s_intrq      = false;
static int  s_intrq_count = 0;
static bool s_drq        = false;
static int  s_drq_count  = 0;

static int  s_seek_drive = -1;
static int  s_seek_track = -1;

/* Fake sector data for read tests */
static uint8_t  s_sector_data[256];
static uint16_t s_sector_len   = 0;
static int      s_read_rc      = 0;   /* return code for read callback */
static bool     s_write_called = false;
static uint8_t  s_written_buf[256];
static uint16_t s_written_len  = 0;
static int      s_write_rc     = 0;

static void cb_irq(void *ctx, bool state)
{
    (void)ctx;
    s_intrq = state;
    if (state) s_intrq_count++;
}

static void cb_drq(void *ctx, bool state)
{
    (void)ctx;
    s_drq = state;
    if (state) s_drq_count++;
}

static void cb_seek(void *ctx, uint8_t drive, uint8_t track)
{
    (void)ctx;
    s_seek_drive = drive;
    s_seek_track = track;
}

static int cb_read_sector(void *ctx,
                          uint8_t drive, uint8_t track, uint8_t sector,
                          uint8_t side, uint8_t density,
                          uint8_t *buf, uint16_t *len)
{
    (void)ctx; (void)drive; (void)track; (void)sector;
    (void)side; (void)density;
    if (s_read_rc != 0) return s_read_rc;
    memcpy(buf, s_sector_data, s_sector_len);
    *len = s_sector_len;
    return 0;
}

static int cb_write_sector(void *ctx,
                           uint8_t drive, uint8_t track, uint8_t sector,
                           uint8_t side, uint8_t density, bool deleted,
                           const uint8_t *buf, uint16_t len)
{
    (void)ctx; (void)drive; (void)track; (void)sector;
    (void)side; (void)density; (void)deleted;
    s_write_called = true;
    s_written_len  = len;
    if (len > sizeof(s_written_buf)) len = (uint16_t)sizeof(s_written_buf);
    memcpy(s_written_buf, buf, len);
    return s_write_rc;
}

static void cb_reset(void)
{
    s_intrq       = false;
    s_intrq_count = 0;
    s_drq         = false;
    s_drq_count   = 0;
    s_seek_drive  = -1;
    s_seek_track  = -1;
    s_read_rc     = 0;
    s_write_called = false;
    s_written_len  = 0;
    s_write_rc     = 0;
    memset(s_sector_data, 0, sizeof(s_sector_data));
    s_sector_len   = 0;
}

/* -------------------------------------------------------------------------
 * Helper: init a fully-wired FDC
 * ------------------------------------------------------------------------- */
static void fdc_setup(wd1770_t *fdc)
{
    cb_reset();
    wd1770_callbacks_t cb = {0};
    cb.irq          = cb_irq;
    cb.drq          = cb_drq;
    cb.seek         = cb_seek;
    cb.read_sector  = cb_read_sector;
    cb.write_sector = cb_write_sector;
    wd1770_init(fdc, &cb);
}

/* Helper: run the FDC for N cycles */
static void tick(wd1770_t *fdc, int n)
{
    for (int i = 0; i < n; i++)
        wd1770_tick(fdc, 1);
}

/* Helper: run until INTRQ fires or limit reached; return cycles taken */
static int tick_until_intrq(wd1770_t *fdc, int limit)
{
    for (int i = 0; i < limit; i++) {
        wd1770_tick(fdc, 1);
        if (s_intrq) return i + 1;
    }
    return limit;
}

/* =========================================================================
 * RESET
 * ========================================================================= */

/*
 * After reset: status=0, track=0, sector=1, data=0, BUSY clear.
 */
static void test_reset_state(void)
{
    wd1770_t fdc;
    fdc_setup(&fdc);

    ASSERT_EQ(fdc.status, 0);
    ASSERT_EQ(fdc.track,  0);
    ASSERT_EQ(fdc.sector, 1);
    ASSERT_EQ(fdc.data,   0);
    ASSERT_FALSE(fdc.status & WD1770_STATUS_BUSY);
    ASSERT_FALSE(fdc.drq);
    ASSERT_FALSE(fdc.intrq);

    test_pass();
}

/* =========================================================================
 * TRACK / SECTOR / DATA REGISTER WRITE
 * ========================================================================= */

/*
 * Writing track/sector/data registers (not busy) updates the values.
 */
static void test_register_writes(void)
{
    wd1770_t fdc;
    fdc_setup(&fdc);

    wd1770_write(&fdc, 1, 42);   /* track */
    wd1770_write(&fdc, 2, 7);    /* sector */
    wd1770_write(&fdc, 3, 0xAB); /* data */

    ASSERT_EQ(wd1770_read(&fdc, 1), 42);
    ASSERT_EQ(wd1770_read(&fdc, 2), 7);
    ASSERT_EQ(fdc.data, 0xAB);

    test_pass();
}

/* =========================================================================
 * RESTORE
 * ========================================================================= */

/*
 * Restore command: BUSY set immediately, after delay head moves to track 0
 * and INTRQ fires.
 */
static void test_restore(void)
{
    wd1770_t fdc;
    fdc_setup(&fdc);

    /* Put the head on track 10 */
    wd1770_write(&fdc, 1, 10);

    /* Issue Restore command (0x00) */
    wd1770_write(&fdc, 0, 0x00);
    ASSERT_TRUE(fdc.status & WD1770_STATUS_BUSY);
    ASSERT_FALSE(s_intrq);

    /* Run until INTRQ */
    tick_until_intrq(&fdc, 100000);
    ASSERT_TRUE(s_intrq);
    ASSERT_EQ(fdc.track, 0);
    ASSERT_EQ(s_seek_track, 0);
    ASSERT_FALSE(fdc.status & WD1770_STATUS_BUSY);

    /* Status register: Type I, at track 0 → TRACK0 bit should be set */
    uint8_t status = wd1770_read(&fdc, 0);
    ASSERT_TRUE(status & WD1770_STATUS_TRACK0);

    test_pass();
}

/* =========================================================================
 * SEEK
 * ========================================================================= */

/*
 * Seek command: head moves to the track in the data register.
 *
 * The WD1770 Seek command works as follows:
 *   1. Issue Seek (0x10) — FDC enters DELAY_SEEK_ALLOW window, seek_ok=false.
 *   2. During that window, CPU writes the target track to the data register
 *      (this sets seek_ok=true).
 *   3. After the allow window expires, cmd_next fires: if seek_ok it moves the
 *      head, else it ignores.
 */
static void test_seek(void)
{
    wd1770_t fdc;
    fdc_setup(&fdc);

    wd1770_write(&fdc, 1, 0);    /* track reg = 0 */
    wd1770_write(&fdc, 0, 0x10); /* Seek command */
    ASSERT_TRUE(fdc.status & WD1770_STATUS_BUSY);

    /* Let cmd_start fire (DELAY_CMD_START=32 cycles) — this resets seek_ok and
     * starts the DELAY_SEEK_ALLOW=800 cycle window */
    tick(&fdc, 40);

    /* Write target track to data register during the allow window */
    wd1770_write(&fdc, 3, 20);   /* data reg = target track 20, sets seek_ok */

    tick_until_intrq(&fdc, 100000);
    ASSERT_TRUE(s_intrq);
    ASSERT_EQ(fdc.track,    20);
    ASSERT_EQ(s_seek_track, 20);
    ASSERT_FALSE(fdc.status & WD1770_STATUS_BUSY);

    test_pass();
}

/* =========================================================================
 * STEP IN / STEP OUT
 * ========================================================================= */

/*
 * Step In (with update): increments track register.
 */
static void test_step_in_update(void)
{
    wd1770_t fdc;
    fdc_setup(&fdc);

    wd1770_write(&fdc, 1, 5);    /* track = 5 */
    wd1770_write(&fdc, 0, 0x50); /* Step In with update (0x50) */

    tick_until_intrq(&fdc, 100000);
    ASSERT_TRUE(s_intrq);
    ASSERT_EQ(fdc.track, 6);    /* incremented */

    test_pass();
}

/*
 * Step Out (with update): decrements track register.
 */
static void test_step_out_update(void)
{
    wd1770_t fdc;
    fdc_setup(&fdc);

    wd1770_write(&fdc, 1, 5);    /* track = 5 */
    wd1770_write(&fdc, 0, 0x70); /* Step Out with update (0x70) */

    tick_until_intrq(&fdc, 100000);
    ASSERT_TRUE(s_intrq);
    ASSERT_EQ(fdc.track, 4);    /* decremented */

    test_pass();
}

/*
 * Step Out at track 0 does not underflow.
 */
static void test_step_out_at_track0(void)
{
    wd1770_t fdc;
    fdc_setup(&fdc);

    wd1770_write(&fdc, 1, 0);    /* track = 0 */
    wd1770_write(&fdc, 0, 0x70); /* Step Out with update */

    tick_until_intrq(&fdc, 100000);
    ASSERT_EQ(fdc.track, 0);    /* clamped at 0 */

    test_pass();
}

/* =========================================================================
 * READ SECTOR
 * ========================================================================= */

/*
 * Read sector: DRQ fires, all 256 bytes are readable via data register,
 * INTRQ fires when done.
 */
static void test_read_sector(void)
{
    wd1770_t fdc;
    fdc_setup(&fdc);

    /* Prepare fake sector data */
    s_sector_len = 256;
    for (int i = 0; i < 256; i++)
        s_sector_data[i] = (uint8_t)(i ^ 0xA5);

    wd1770_write(&fdc, 1, 3);    /* track  */
    wd1770_write(&fdc, 2, 1);    /* sector */
    wd1770_write(&fdc, 0, 0x80); /* Read single sector */

    /* Run until first DRQ */
    int drq_count_before = s_drq_count;
    tick(&fdc, 100);
    ASSERT_TRUE(s_drq || s_drq_count > drq_count_before);

    /* Drain all 256 bytes */
    uint8_t received[256];
    for (int i = 0; i < 256; i++)
        received[i] = wd1770_read(&fdc, 3);

    /* Run until INTRQ */
    tick_until_intrq(&fdc, 1000);
    ASSERT_TRUE(s_intrq);
    ASSERT_FALSE(fdc.status & WD1770_STATUS_BUSY);

    /* Verify data matches */
    for (int i = 0; i < 256; i++)
        ASSERT_EQ(received[i], (uint8_t)(i ^ 0xA5));

    test_pass();
}

/*
 * Read sector: RNF when callback returns error.
 */
static void test_read_sector_rnf(void)
{
    wd1770_t fdc;
    fdc_setup(&fdc);

    s_read_rc    = -1;  /* force error */
    s_sector_len = 0;

    wd1770_write(&fdc, 1, 3);
    wd1770_write(&fdc, 2, 1);
    wd1770_write(&fdc, 0, 0x80); /* Read single sector */

    tick_until_intrq(&fdc, 1000);
    ASSERT_TRUE(s_intrq);
    ASSERT_TRUE(fdc.status & WD1770_STATUS_RNF);
    ASSERT_FALSE(fdc.status & WD1770_STATUS_BUSY);

    test_pass();
}

/* =========================================================================
 * WRITE SECTOR
 * ========================================================================= */

/*
 * Write sector: DRQ fires, CPU writes bytes via data register,
 * write callback invoked with the buffered data on completion.
 */
static void test_write_sector(void)
{
    wd1770_t fdc;
    fdc_setup(&fdc);

    wd1770_write(&fdc, 1, 3);    /* track  */
    wd1770_write(&fdc, 2, 1);    /* sector */
    wd1770_write(&fdc, 0, 0xA0); /* Write single sector */

    /* FDC should be busy and DRQ should fire */
    tick(&fdc, 100);
    ASSERT_TRUE(fdc.status & WD1770_STATUS_BUSY);

    /* Write 256 bytes */
    for (int i = 0; i < 256; i++) {
        wd1770_write(&fdc, 3, (uint8_t)(i + 1));
    }

    /* Run until completion */
    tick_until_intrq(&fdc, 1000);
    ASSERT_TRUE(s_intrq);
    ASSERT_TRUE(s_write_called);
    ASSERT_FALSE(fdc.status & WD1770_STATUS_BUSY);

    /* Verify first and last bytes */
    ASSERT_EQ(s_written_buf[0],   1);
    ASSERT_EQ(s_written_buf[255], 256 & 0xFF);

    test_pass();
}

/*
 * Write sector: write-protected disk → WRITE_PROT status, no write callback.
 */
static void test_write_sector_protected(void)
{
    wd1770_t fdc;
    fdc_setup(&fdc);
    fdc.write_protect = true;

    wd1770_write(&fdc, 1, 3);
    wd1770_write(&fdc, 2, 1);
    wd1770_write(&fdc, 0, 0xA0); /* Write single sector */

    tick_until_intrq(&fdc, 1000);
    ASSERT_TRUE(s_intrq);
    ASSERT_TRUE(fdc.status & WD1770_STATUS_WRITE_PROT);
    ASSERT_FALSE(fdc.status & WD1770_STATUS_BUSY);
    ASSERT_FALSE(s_write_called);

    test_pass();
}

/* =========================================================================
 * FORCE INTERRUPT
 * ========================================================================= */

/*
 * Force interrupt (0xD0): if idle, sets motor on and fires INTRQ.
 */
static void test_force_interrupt_idle(void)
{
    wd1770_t fdc;
    fdc_setup(&fdc);

    ASSERT_FALSE(fdc.status & WD1770_STATUS_BUSY);
    wd1770_write(&fdc, 0, 0xD0); /* Force interrupt */

    tick_until_intrq(&fdc, 1000);
    ASSERT_TRUE(s_intrq);
    ASSERT_FALSE(fdc.status & WD1770_STATUS_BUSY);

    test_pass();
}

/*
 * Force interrupt: aborts a running command (e.g. restore).
 */
static void test_force_interrupt_abort(void)
{
    wd1770_t fdc;
    fdc_setup(&fdc);

    /* Start a seek (long delay) */
    wd1770_write(&fdc, 1, 0);
    wd1770_write(&fdc, 3, 50);
    wd1770_write(&fdc, 0, 0x10); /* Seek */
    ASSERT_TRUE(fdc.status & WD1770_STATUS_BUSY);

    /* Tick a bit but not enough to complete */
    tick(&fdc, 50);

    /* Force interrupt */
    wd1770_write(&fdc, 0, 0xD0);
    tick_until_intrq(&fdc, 1000);
    ASSERT_TRUE(s_intrq);
    ASSERT_FALSE(fdc.status & WD1770_STATUS_BUSY);

    test_pass();
}

/* =========================================================================
 * STATUS REGISTER
 * ========================================================================= */

/*
 * Status register: reading clears INTRQ.
 */
static void test_status_read_clears_intrq(void)
{
    wd1770_t fdc;
    fdc_setup(&fdc);

    wd1770_write(&fdc, 1, 0);
    wd1770_write(&fdc, 0, 0x00); /* Restore */
    tick_until_intrq(&fdc, 100000);
    ASSERT_TRUE(s_intrq);

    /* Reading status clears INTRQ */
    (void)wd1770_read(&fdc, 0);
    ASSERT_FALSE(s_intrq);
    ASSERT_FALSE(wd1770_get_intrq(&fdc));

    test_pass();
}

/*
 * Status register: Type I after restore shows TRACK0 when at track 0.
 */
static void test_status_type1_track0(void)
{
    wd1770_t fdc;
    fdc_setup(&fdc);

    wd1770_write(&fdc, 1, 5);    /* set track != 0 */
    wd1770_write(&fdc, 0, 0x00); /* Restore */
    tick_until_intrq(&fdc, 100000);

    uint8_t s = wd1770_read(&fdc, 0);
    ASSERT_TRUE(s & WD1770_STATUS_TRACK0);

    test_pass();
}

/* =========================================================================
 * STUBBED COMMANDS
 * ========================================================================= */

/*
 * Read address: stubbed → RNF after delay.
 */
static void test_read_address_stub(void)
{
    wd1770_t fdc;
    fdc_setup(&fdc);

    wd1770_write(&fdc, 0, 0xC0); /* Read address */
    tick_until_intrq(&fdc, 10000);
    ASSERT_TRUE(s_intrq);
    ASSERT_TRUE(fdc.status & WD1770_STATUS_RNF);

    test_pass();
}

/*
 * Read track: stubbed → RNF after delay.
 */
static void test_read_track_stub(void)
{
    wd1770_t fdc;
    fdc_setup(&fdc);

    wd1770_write(&fdc, 0, 0xE0); /* Read track */
    tick_until_intrq(&fdc, 10000);
    ASSERT_TRUE(s_intrq);
    ASSERT_TRUE(fdc.status & WD1770_STATUS_RNF);

    test_pass();
}

/* =========================================================================
 * MULTI-SECTOR READ
 * ========================================================================= */

/*
 * Read multiple sectors (0x90): reads sectors until stopped.
 *
 * Real WD1770 multi-sector reads continue indefinitely until the CPU issues
 * a Force Interrupt (0xD0) or a disk error occurs.  This test verifies:
 *   - sector 1 is read and DRQ fires
 *   - after draining sector 1, the FDC automatically reads sector 2 (DRQ fires again)
 *   - issuing Force Interrupt aborts the command cleanly
 */
static void test_read_multi_sector(void)
{
    wd1770_t fdc;
    fdc_setup(&fdc);

    /* Always return 4-byte "sectors" */
    s_sector_len = 4;
    s_sector_data[0] = 0xBB;
    s_sector_data[1] = 0xCC;
    s_sector_data[2] = 0xDD;
    s_sector_data[3] = 0xEE;

    wd1770_write(&fdc, 1, 0);    /* track 0 */
    wd1770_write(&fdc, 2, 1);    /* start sector 1 */
    wd1770_write(&fdc, 0, 0x90); /* Read multiple sectors */

    /* Drain sector 1: wait for all 4 DRQ pulses */
    for (int drain = 0; drain < 4; drain++) {
        /* Wait for DRQ */
        for (int i = 0; i < 10000 && !s_drq; i++)
            wd1770_tick(&fdc, 1);
        ASSERT_TRUE(s_drq);
        (void)wd1770_read(&fdc, 3);
    }

    /* After draining sector 1, FDC should move to sector 2 — wait for next DRQ */
    int drq_before_sector2 = s_drq_count;
    for (int i = 0; i < 20000 && s_drq_count == drq_before_sector2; i++)
        wd1770_tick(&fdc, 1);
    ASSERT_TRUE(s_drq_count > drq_before_sector2); /* sector 2 DRQ fired */

    /* Stop the multi-sector read with Force Interrupt */
    wd1770_write(&fdc, 0, 0xD0);
    tick_until_intrq(&fdc, 2000);
    ASSERT_TRUE(s_intrq);
    ASSERT_FALSE(fdc.status & WD1770_STATUS_BUSY);

    test_pass();
}

/* =========================================================================
 * SELECT (drive/side/density)
 * ========================================================================= */

/*
 * wd1770_select() stores the drive, side, density in the struct.
 */
static void test_select(void)
{
    wd1770_t fdc;
    fdc_setup(&fdc);

    wd1770_select(&fdc, 1, 1, WD1770_DENSITY_MFM);
    ASSERT_EQ(fdc.cur_drive, 1);
    ASSERT_EQ(fdc.cur_side,  1);
    ASSERT_EQ(fdc.density,   WD1770_DENSITY_MFM);

    wd1770_select(&fdc, 0, 0, WD1770_DENSITY_FM);
    ASSERT_EQ(fdc.cur_drive, 0);
    ASSERT_EQ(fdc.cur_side,  0);
    ASSERT_EQ(fdc.density,   WD1770_DENSITY_FM);

    test_pass();
}

/* =========================================================================
 * BUSY GUARD
 * ========================================================================= */

/*
 * While BUSY, writing track/sector registers is ignored.
 */
static void test_busy_rejects_track_write(void)
{
    wd1770_t fdc;
    fdc_setup(&fdc);

    wd1770_write(&fdc, 1, 0);
    wd1770_write(&fdc, 3, 30);
    wd1770_write(&fdc, 0, 0x10); /* Seek → BUSY */
    ASSERT_TRUE(fdc.status & WD1770_STATUS_BUSY);

    /* Try to overwrite track register while busy */
    wd1770_write(&fdc, 1, 99);
    ASSERT_EQ(fdc.track, 0);   /* must be unchanged */

    test_pass();
}

/* =========================================================================
 * main
 * ========================================================================= */
int main(void)
{
    printf("=== wd1770 unit tests ===\n");

    TEST(reset_state);
    TEST(register_writes);
    TEST(restore);
    TEST(seek);
    TEST(step_in_update);
    TEST(step_out_update);
    TEST(step_out_at_track0);
    TEST(read_sector);
    TEST(read_sector_rnf);
    TEST(write_sector);
    TEST(write_sector_protected);
    TEST(force_interrupt_idle);
    TEST(force_interrupt_abort);
    TEST(status_read_clears_intrq);
    TEST(status_type1_track0);
    TEST(read_address_stub);
    TEST(read_track_stub);
    TEST(read_multi_sector);
    TEST(select);
    TEST(busy_rejects_track_write);

    printf("\n%d/%d tests passed\n", s_tests_run - s_tests_failed, s_tests_run);
    return (s_tests_failed == 0) ? 0 : 1;
}
