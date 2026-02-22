/*
 * test_acia.c — Unit tests for bbc_tape ACIA emulation
 *
 * Tests are derived from the MC6850 Verilog reference implementation by
 * Gyorgy Szombathelyi (GPL-2.0), cross-checked against BBC Micro MOS 1.2
 * tape loading behaviour observed during emulator development.
 *
 * MC6850 control register layout (write to $FE08):
 *   bits 1:0 = clk_mult / master reset  (11 = master reset)
 *   bit  2   = parity_odd
 *   bits 4:3 = word select
 *   bits 6:5 = TX control
 *   bit  7   = rx_ie  (RX interrupt enable)
 *
 * MC6850 status register layout (read from $FE08):
 *   bit 0 = RDRF   receive data register full
 *   bit 1 = TDRE   transmit data register empty (always 1 — no TX)
 *   bit 2 = DCD    data carrier detect (0=carrier/motor on, 1=no carrier)
 *   bit 3 = CTS    clear to send (always 0)
 *   bit 4 = FE     framing error (always 0)
 *   bit 5 = OVRN   overrun (always 0)
 *   bit 6 = PE     parity error (always 0)
 *   bit 7 = IRQ    (~irq_n) = rx_ie & (RDRF | OVRN)
 *
 * BBC MOS 1.2 initialisation sequence (from FB50):
 *   STA $FE10  #$05   — Serial ULA: motor ON, 1200 baud
 *   STA $FE10  #$85   — Serial ULA: motor ON + RS423 flag
 *   LDA #$D0 | $C6    — typically $D6 = 1101 0110
 *   STA $FE08         — ACIA control: rx_ie=1, 8N2
 *
 * Build and run:
 *   make -C tests/bbc_tape && tests/bbc_tape/test_acia
 *
 * Licence: GPL-2.0
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>

#include "../../components/bbc_tape/include/bbc_tape.h"

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
        printf("  %-50s", #name); \
        test_##name(); \
        if (s_tests_failed == 0 || \
            /* recheck: did this test add a failure? */ \
            s_tests_failed == _prev_failed) \
            printf("OK\n"); \
        _prev_failed = s_tests_failed; \
    } while(0)

static int _prev_failed = 0;

#define ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            printf("FAIL\n"); \
            fprintf(stderr, "    FAIL [%s] line %d: " #a " = 0x%02X, expected 0x%02X\n", \
                    s_current_test, __LINE__, (unsigned)(a), (unsigned)(b)); \
            s_tests_failed++; \
            return; \
        } \
    } while(0)

#define ASSERT_TRUE(x) \
    do { \
        if (!(x)) { \
            printf("FAIL\n"); \
            fprintf(stderr, "    FAIL [%s] line %d: " #x " is false\n", \
                    s_current_test, __LINE__); \
            s_tests_failed++; \
            return; \
        } \
    } while(0)

#define ASSERT_FALSE(x) ASSERT_TRUE(!(x))

/* -------------------------------------------------------------------------
 * IRQ tracking helper
 * ------------------------------------------------------------------------- */
static bool s_irq_state = false;
static int  s_irq_count = 0;

static void irq_cb(void *ctx, bool state)
{
    (void)ctx;
    s_irq_state = state;
    if (state) s_irq_count++;
}

static void reset_irq(void)
{
    s_irq_state = false;
    s_irq_count = 0;
}

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */
#define STATUS_RDRF  0x01
#define STATUS_TDRE  0x02
#define STATUS_DCD   0x04
#define STATUS_CTS   0x08
#define STATUS_FE    0x10
#define STATUS_OVRN  0x20
#define STATUS_PE    0x40
#define STATUS_IRQ   0x80

#define CTRL_RESET   0x03   /* bits 1:0 = 11 → master reset */
#define CTRL_RX_IRQ  0x80   /* bit 7 = rx_ie */

/* Advance tape by N cycles — enough for one byte at 1200 baud (1667 cycles) */
#define ONE_BYTE_CYCLES  1667
#define HALF_BYTE_CYCLES  833

static void tape_init_with_irq(bbc_tape_t *t)
{
    bbc_tape_init(t);
    bbc_tape_set_irq_cb(t, irq_cb, NULL);
    reset_irq();
}

/* =========================================================================
 * Test: power-on reset state
 * ========================================================================= */
static void test_reset_state(void)
{
    bbc_tape_t t;
    tape_init_with_irq(&t);

    /* After init: motor off, no byte ready */
    uint8_t st = bbc_tape_read(&t, 0);

    /* TDRE=1 always, DCD=1 (motor off = no carrier), RDRF=0, IRQ=0 */
    ASSERT_EQ(st & STATUS_RDRF, 0);
    ASSERT_EQ(st & STATUS_TDRE, STATUS_TDRE);
    ASSERT_EQ(st & STATUS_DCD,  STATUS_DCD);
    ASSERT_EQ(st & STATUS_IRQ,  0);
    ASSERT_FALSE(s_irq_state);
}

/* =========================================================================
 * Test: master reset (ctrl bits 1:0 = 11)
 * ========================================================================= */
static void test_master_reset(void)
{
    bbc_tape_t t;
    tape_init_with_irq(&t);

    /* Write master reset */
    bbc_tape_write(&t, 0, CTRL_RESET);

    uint8_t st = bbc_tape_read(&t, 0);
    ASSERT_EQ(st & STATUS_RDRF, 0);
    ASSERT_EQ(st & STATUS_TDRE, STATUS_TDRE);
    /* IRQ must be deasserted after master reset */
    ASSERT_EQ(st & STATUS_IRQ, 0);
    ASSERT_FALSE(s_irq_state);
}

/* =========================================================================
 * Test: master reset clears a pending byte
 * ========================================================================= */
static void test_master_reset_clears_rdrf(void)
{
    bbc_tape_t t;
    tape_init_with_irq(&t);

    /* Load a single-byte block */
    uint8_t buf[] = { 0xAA };
    bbc_tape_load_buffer(&t, buf, sizeof(buf));
    bbc_tape_set_motor(&t, true);

    /* Enable IRQ and advance until byte delivered */
    bbc_tape_write(&t, 0, CTRL_RX_IRQ | 0x10); /* rx_ie=1, 8N1 */
    bbc_tape_tick(&t, ONE_BYTE_CYCLES * 2);      /* past startup delay + 1 byte */

    /* Byte should be ready */
    ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_RDRF, STATUS_RDRF);

    /* Master reset must clear it */
    bbc_tape_write(&t, 0, CTRL_RESET);
    ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_RDRF, 0);
    ASSERT_FALSE(s_irq_state);
}

/* =========================================================================
 * Test: motor off → DCD=1 (no carrier)
 * ========================================================================= */
static void test_motor_off_dcd(void)
{
    bbc_tape_t t;
    tape_init_with_irq(&t);

    bbc_tape_set_motor(&t, false);
    ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_DCD, STATUS_DCD);
}

/* =========================================================================
 * Test: motor on → DCD=0 (carrier present)
 * ========================================================================= */
static void test_motor_on_dcd(void)
{
    bbc_tape_t t;
    tape_init_with_irq(&t);

    bbc_tape_set_motor(&t, true);
    ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_DCD, 0);
}

/* =========================================================================
 * Test: TDRE always 1 (we never transmit)
 * ========================================================================= */
static void test_tdre_always_set(void)
{
    bbc_tape_t t;
    tape_init_with_irq(&t);

    ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_TDRE, STATUS_TDRE);
    bbc_tape_set_motor(&t, true);
    ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_TDRE, STATUS_TDRE);
    bbc_tape_write(&t, 1, 0x55); /* TX write ignored */
    ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_TDRE, STATUS_TDRE);
}

/* =========================================================================
 * Test: RX IRQ disabled by default (bit 7 = 0)
 * ========================================================================= */
static void test_irq_disabled_by_default(void)
{
    bbc_tape_t t;
    tape_init_with_irq(&t);

    uint8_t buf[] = { 0x2A, 0x55 };
    bbc_tape_load_buffer(&t, buf, sizeof(buf));
    bbc_tape_set_motor(&t, true);

    /* No rx_ie set → even after byte arrives, IRQ must not fire */
    bbc_tape_write(&t, 0, 0x10); /* rx_ie=0, 8N1 */
    bbc_tape_tick(&t, ONE_BYTE_CYCLES * 10);

    ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_IRQ, 0);
    ASSERT_FALSE(s_irq_state);
}

/* =========================================================================
 * Test: RX IRQ fires when rx_ie=1 and byte arrives
 * ========================================================================= */
static void test_irq_fires_on_byte_received(void)
{
    bbc_tape_t t;
    tape_init_with_irq(&t);

    uint8_t buf[] = { 0x2A };
    bbc_tape_load_buffer(&t, buf, sizeof(buf));
    bbc_tape_set_motor(&t, true);

    bbc_tape_write(&t, 0, CTRL_RX_IRQ | 0x10); /* rx_ie=1 */
    reset_irq();

    /* Tick past startup delay + one byte period */
    bbc_tape_tick(&t, ONE_BYTE_CYCLES * 10);

    ASSERT_TRUE(s_irq_state);
    ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_IRQ, STATUS_IRQ);
    ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_RDRF, STATUS_RDRF);
}

/* =========================================================================
 * Test: IRQ deasserts when RDR is read
 * ========================================================================= */
static void test_irq_clears_on_rdr_read(void)
{
    bbc_tape_t t;
    tape_init_with_irq(&t);

    uint8_t buf[] = { 0x2A };
    bbc_tape_load_buffer(&t, buf, sizeof(buf));
    bbc_tape_set_motor(&t, true);

    bbc_tape_write(&t, 0, CTRL_RX_IRQ | 0x10);
    bbc_tape_tick(&t, ONE_BYTE_CYCLES * 10);

    /* IRQ should be asserted */
    ASSERT_TRUE(s_irq_state);

    /* Read RDR ($FE09) — must clear RDRF and deassert IRQ */
    uint8_t data = bbc_tape_read(&t, 1);
    ASSERT_EQ(data, 0x2A);
    ASSERT_FALSE(s_irq_state);
    ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_RDRF, 0);
    ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_IRQ,  0);
}

/* =========================================================================
 * Test: enabling rx_ie when byte already pending asserts IRQ immediately
 * (matches Verilog: irq_n <= ~(rx_ie && (rx_full | rx_ovr)) is combinational)
 * ========================================================================= */
static void test_irq_asserts_when_rxie_enabled_with_pending_byte(void)
{
    bbc_tape_t t;
    tape_init_with_irq(&t);

    uint8_t buf[] = { 0x2A };
    bbc_tape_load_buffer(&t, buf, sizeof(buf));
    bbc_tape_set_motor(&t, true);

    /* IRQ disabled initially */
    bbc_tape_write(&t, 0, 0x10); /* rx_ie=0 */
    bbc_tape_tick(&t, ONE_BYTE_CYCLES * 10);

    /* Byte arrived but IRQ not yet asserted */
    ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_RDRF, STATUS_RDRF);
    ASSERT_FALSE(s_irq_state);

    reset_irq();

    /* Now enable rx_ie — IRQ must fire immediately */
    bbc_tape_write(&t, 0, CTRL_RX_IRQ | 0x10);
    ASSERT_TRUE(s_irq_state);
    ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_IRQ, STATUS_IRQ);
}

/* =========================================================================
 * Test: byte-by-byte delivery order matches block data
 * ========================================================================= */
static void test_byte_delivery_order(void)
{
    bbc_tape_t t;
    tape_init_with_irq(&t);

    uint8_t buf[] = { 0x2A, 0x41, 0x44, 0x56, 0x52 };
    bbc_tape_load_buffer(&t, buf, sizeof(buf));
    bbc_tape_set_motor(&t, true);

    bbc_tape_write(&t, 0, CTRL_RX_IRQ | 0x10);

    for (int i = 0; i < (int)sizeof(buf); i++) {
        /* Tick until byte arrives */
        int ticks = 0;
        while (!(bbc_tape_read(&t, 0) & STATUS_RDRF)) {
            bbc_tape_tick(&t, 100);
            if (++ticks > 10000) break; /* timeout */
        }
        ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_RDRF, STATUS_RDRF);
        uint8_t d = bbc_tape_read(&t, 1);
        ASSERT_EQ(d, buf[i]);
    }
}

/* =========================================================================
 * Test: no byte delivered while RDRF set (no overrun — MOS must read first)
 * ========================================================================= */
static void test_no_overrun_while_rdrf_set(void)
{
    bbc_tape_t t;
    tape_init_with_irq(&t);

    uint8_t buf[] = { 0xAA, 0xBB };
    bbc_tape_load_buffer(&t, buf, sizeof(buf));
    bbc_tape_set_motor(&t, true);
    bbc_tape_write(&t, 0, CTRL_RX_IRQ | 0x10);

    /* Wait for first byte */
    int ticks = 0;
    while (!(bbc_tape_read(&t, 0) & STATUS_RDRF)) {
        bbc_tape_tick(&t, 100);
        if (++ticks > 10000) break;
    }
    ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_RDRF, STATUS_RDRF);

    /* Tick several more byte periods WITHOUT reading — second byte must not
     * overwrite the first (our implementation holds until MOS reads) */
    bbc_tape_tick(&t, ONE_BYTE_CYCLES * 5);

    /* First byte must still be 0xAA */
    ASSERT_EQ(bbc_tape_read(&t, 1), 0xAA);
}

/* =========================================================================
 * Test: MOS BBC init sequence (master-reset then ctrl=$D6)
 * ========================================================================= */
static void test_mos_init_sequence(void)
{
    bbc_tape_t t;
    tape_init_with_irq(&t);

    /* MOS writes $03 (master reset) */
    bbc_tape_write(&t, 0, 0x03);
    ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_RDRF, 0);
    ASSERT_FALSE(s_irq_state);

    /* MOS writes $56 (no rx_ie, 8N2) — as observed in log */
    bbc_tape_write(&t, 0, 0x56);
    ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_IRQ, 0);

    /* MOS writes $D6 (rx_ie=1, 8N2) — the correct production value */
    bbc_tape_write(&t, 0, 0xD6);

    /* Now load a byte and check IRQ fires */
    uint8_t buf[] = { 0x2A };
    bbc_tape_load_buffer(&t, buf, sizeof(buf));
    bbc_tape_set_motor(&t, true);
    reset_irq();

    bbc_tape_tick(&t, ONE_BYTE_CYCLES * 10);

    ASSERT_TRUE(s_irq_state);
    ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_RDRF, STATUS_RDRF);
    ASSERT_EQ(bbc_tape_read(&t, 1), 0x2A);
}

/* =========================================================================
 * Test: ctrl=$56 (rx_ie=0) — no IRQ, but RDRF still visible via polling
 * This matches the behaviour observed in MOS tape initialisation.
 * ========================================================================= */
static void test_ctrl_56_polling_works(void)
{
    bbc_tape_t t;
    tape_init_with_irq(&t);

    bbc_tape_write(&t, 0, 0x56); /* rx_ie=0 */

    uint8_t buf[] = { 0x2A, 0x41 };
    bbc_tape_load_buffer(&t, buf, sizeof(buf));
    bbc_tape_set_motor(&t, true);

    /* Tick until byte arrives */
    int ticks = 0;
    while (!(bbc_tape_read(&t, 0) & STATUS_RDRF)) {
        bbc_tape_tick(&t, 100);
        if (++ticks > 10000) break;
    }

    /* RDRF visible even without IRQ */
    ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_RDRF, STATUS_RDRF);
    /* No IRQ */
    ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_IRQ, 0);
    ASSERT_FALSE(s_irq_state);
    /* Data readable */
    ASSERT_EQ(bbc_tape_read(&t, 1), 0x2A);
}

/* =========================================================================
 * Test: multi-block stream — bytes from block 0 then block 1
 * ========================================================================= */
static void test_multiblock_stream(void)
{
    bbc_tape_t t;
    tape_init_with_irq(&t);

    /* Two blocks of 2 bytes each */
    uint8_t b0[] = { 0x11, 0x22 };
    uint8_t b1[] = { 0x33, 0x44 };

    bbc_tape_load_buffer(&t, b0, sizeof(b0));
    bbc_tape_append_buffer(&t, b1, sizeof(b1));

    bbc_tape_set_motor(&t, true);
    bbc_tape_write(&t, 0, CTRL_RX_IRQ | 0x10);

    uint8_t expected[] = { 0x11, 0x22, 0x33, 0x44 };
    for (int i = 0; i < 4; i++) {
        int ticks = 0;
        while (!(bbc_tape_read(&t, 0) & STATUS_RDRF)) {
            bbc_tape_tick(&t, 100);
            if (++ticks > 10000) break;
        }
        ASSERT_EQ(bbc_tape_read(&t, 1), expected[i]);
    }
}

/* =========================================================================
 * Test: tape end — RDRF stays 0 after last byte consumed
 * ========================================================================= */
static void test_tape_end_no_more_bytes(void)
{
    bbc_tape_t t;
    tape_init_with_irq(&t);

    uint8_t buf[] = { 0xAA };
    bbc_tape_load_buffer(&t, buf, sizeof(buf));
    bbc_tape_set_motor(&t, true);
    bbc_tape_write(&t, 0, CTRL_RX_IRQ | 0x10);

    /* Drain the one byte */
    int ticks = 0;
    while (!(bbc_tape_read(&t, 0) & STATUS_RDRF)) {
        bbc_tape_tick(&t, 100);
        if (++ticks > 10000) break;
    }
    bbc_tape_read(&t, 1); /* consume */

    /* Tick a lot more — no new bytes should arrive */
    bbc_tape_tick(&t, ONE_BYTE_CYCLES * 20);
    ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_RDRF, 0);
    ASSERT_FALSE(s_irq_state);
}

/* =========================================================================
 * Test: startup delay — no byte delivered before motor turns on
 * ========================================================================= */
static void test_no_byte_before_motor_on(void)
{
    bbc_tape_t t;
    tape_init_with_irq(&t);

    uint8_t buf[] = { 0x2A, 0x55 };
    bbc_tape_load_buffer(&t, buf, sizeof(buf));
    /* Motor NOT turned on */
    bbc_tape_write(&t, 0, CTRL_RX_IRQ | 0x10);

    /* Tick a lot — no byte should arrive */
    bbc_tape_tick(&t, ONE_BYTE_CYCLES * 100);

    ASSERT_EQ(bbc_tape_read(&t, 0) & STATUS_RDRF, 0);
    ASSERT_FALSE(s_irq_state);
}

/* =========================================================================
 * Test: motor toggle — motor off does not reset position
 * After motor on → off → on, stream continues from same position.
 * ========================================================================= */
static void test_motor_toggle_continues_stream(void)
{
    bbc_tape_t t;
    tape_init_with_irq(&t);

    uint8_t buf[] = { 0xAA, 0xBB, 0xCC };
    bbc_tape_load_buffer(&t, buf, sizeof(buf));
    bbc_tape_set_motor(&t, true);
    bbc_tape_write(&t, 0, CTRL_RX_IRQ | 0x10);

    /* Read first byte */
    int ticks = 0;
    while (!(bbc_tape_read(&t, 0) & STATUS_RDRF)) {
        bbc_tape_tick(&t, 100);
        if (++ticks > 10000) break;
    }
    ASSERT_EQ(bbc_tape_read(&t, 1), 0xAA);

    /* Motor off */
    bbc_tape_set_motor(&t, false);
    bbc_tape_tick(&t, ONE_BYTE_CYCLES * 2);

    /* Motor on again */
    bbc_tape_set_motor(&t, true);
    bbc_tape_write(&t, 0, CTRL_RX_IRQ | 0x10);

    /* Should get 0xBB next (not 0xAA again) */
    ticks = 0;
    while (!(bbc_tape_read(&t, 0) & STATUS_RDRF)) {
        bbc_tape_tick(&t, 100);
        if (++ticks > 10000) break;
    }
    ASSERT_EQ(bbc_tape_read(&t, 1), 0xBB);
}

/* =========================================================================
 * main
 * ========================================================================= */
int main(void)
{
    printf("=== BBC Tape ACIA tests ===\n\n");

    printf("--- Status register ---\n");
    TEST(reset_state);
    TEST(motor_off_dcd);
    TEST(motor_on_dcd);
    TEST(tdre_always_set);

    printf("\n--- Master reset ---\n");
    TEST(master_reset);
    TEST(master_reset_clears_rdrf);

    printf("\n--- IRQ behaviour (MC6850 Verilog: irq_n = ~(rx_ie & (rx_full|ovr))) ---\n");
    TEST(irq_disabled_by_default);
    TEST(irq_fires_on_byte_received);
    TEST(irq_clears_on_rdr_read);
    TEST(irq_asserts_when_rxie_enabled_with_pending_byte);

    printf("\n--- Byte delivery ---\n");
    TEST(byte_delivery_order);
    TEST(no_overrun_while_rdrf_set);
    TEST(tape_end_no_more_bytes);

    printf("\n--- Polling mode (rx_ie=0) ---\n");
    TEST(ctrl_56_polling_works);

    printf("\n--- MOS BBC sequences ---\n");
    TEST(mos_init_sequence);

    printf("\n--- Motor control ---\n");
    TEST(no_byte_before_motor_on);
    TEST(motor_toggle_continues_stream);

    printf("\n--- Multi-block ---\n");
    TEST(multiblock_stream);

    printf("\n=== %d tests run, %d failed ===\n",
           s_tests_run, s_tests_failed);
    return s_tests_failed ? 1 : 0;
}
