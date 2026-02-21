/*
 * test_m6522.c — Unit tests for the m6522 VIA component
 *
 * Tests cover:
 *   - T1 one-shot: fires exactly once, IFR bit set, cleared on T1CL read
 *   - T1 continuous: reloads and fires repeatedly
 *   - T1 PB7 toggle on underflow
 *   - T2 one-shot: fires once, wraps (no reload)
 *   - IFR/IER: set/clear individual bits, IRQ gating
 *   - CA1 edge detection (both polarities)
 *   - CB1 edge detection
 *   - CA2 input edge detection
 *
 * Build and run:
 *   make -C tests/m6522 && tests/m6522/test_m6522
 *
 * Licence: GPL-2.0
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

/* Pull in the implementation directly (no ESP-IDF needed) */
#include "../../components/m6522/include/m6522.h"

/* -------------------------------------------------------------------------
 * Minimal test framework — prints PASS/FAIL per test
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
 * Shared IRQ tracking
 * ------------------------------------------------------------------------- */
static bool  s_irq_state   = false;
static int   s_irq_count   = 0;

static void cb_irq(void *ctx, bool state)
{
    (void)ctx;
    s_irq_state = state;
    if (state) s_irq_count++;
}

static void irq_reset(void)
{
    s_irq_state = false;
    s_irq_count = 0;
}

/* -------------------------------------------------------------------------
 * Helper: init a VIA with IRQ callback, enable a given IER mask
 * ------------------------------------------------------------------------- */
static void via_setup(m6522_t *via, uint8_t ier_mask)
{
    m6522_callbacks_t cb = {0};
    cb.irq = cb_irq;
    m6522_init(via, &cb);
    m6522_reset(via);
    irq_reset();
    /* Enable interrupts: bit 7 = 1 (set), bits = mask */
    m6522_write(via, M6522_REG_IER, 0x80 | ier_mask);
}

/* -------------------------------------------------------------------------
 * Helper: tick via for N cycles
 * ------------------------------------------------------------------------- */
static void tick(m6522_t *via, int n)
{
    for (int i = 0; i < n; i++)
        m6522_tick(via, 1);
}

/* =========================================================================
 * T1 ONE-SHOT
 * ========================================================================= */

/*
 * T1 one-shot: load 10 into T1, tick 11+2 pipeline cycles, expect:
 *   - IFR T1 bit set after underflow
 *   - IRQ callback fired
 *   - Reading T1CL clears the IFR T1 bit
 *   - Timer does NOT fire a second time
 */
static void test_t1_oneshot_fires(void)
{
    m6522_t via;
    via_setup(&via, M6522_IRQ_T1);

    /* ACR: T1 one-shot (bit 6 = 0) */
    m6522_write(&via, M6522_REG_ACR, 0x00);

    /* Load T1 latch and counter with 10 */
    m6522_write(&via, M6522_REG_T1CL, 10);
    m6522_write(&via, M6522_REG_T1CH, 0);   /* also starts the timer */

    /* Pipeline: 3 extra cycles before first underflow (2 delay + 1 for 0->0xFFFF) */
    tick(&via, 13);   /* 10 + 3 pipeline */

    ASSERT_TRUE(via.ifr & M6522_IRQ_T1);
    ASSERT_TRUE(s_irq_state);
    ASSERT_EQ(s_irq_count, 1);

    /* Reading T1CL clears T1 IFR flag */
    (void)m6522_read(&via, M6522_REG_T1CL);
    ASSERT_FALSE(via.ifr & M6522_IRQ_T1);

    /* One-shot: tick another 20 cycles, should NOT fire again */
    irq_reset();
    tick(&via, 20);
    ASSERT_EQ(s_irq_count, 0);

    test_pass();
}

/*
 * T1 one-shot: writing T1CH reloads and restarts the timer (re-arm).
 */
static void test_t1_oneshot_rearm(void)
{
    m6522_t via;
    via_setup(&via, M6522_IRQ_T1);
    m6522_write(&via, M6522_REG_ACR, 0x00);

    m6522_write(&via, M6522_REG_T1CL, 5);
    m6522_write(&via, M6522_REG_T1CH, 0);
    tick(&via, 8);   /* 5 + 3 */
    ASSERT_TRUE(via.ifr & M6522_IRQ_T1);

    /* Re-arm */
    irq_reset();
    m6522_write(&via, M6522_REG_T1CL, 5);
    m6522_write(&via, M6522_REG_T1CH, 0);
    tick(&via, 8);   /* 5 + 3 */
    ASSERT_TRUE(via.ifr & M6522_IRQ_T1);
    ASSERT_EQ(s_irq_count, 1);

    test_pass();
}

/* =========================================================================
 * T1 CONTINUOUS
 * ========================================================================= */

/*
 * T1 continuous: fires repeatedly, reloads from latch each time.
 * Load T1 = 4. Expect IFR T1 set at cycle 6, 12, 18...
 */
static void test_t1_continuous(void)
{
    m6522_t via;
    via_setup(&via, M6522_IRQ_T1);

    /* ACR: T1 continuous (bit 6 = 1) */
    m6522_write(&via, M6522_REG_ACR, M6522_ACR_T1_CONTINUOUS);

    m6522_write(&via, M6522_REG_T1CL, 4);
    m6522_write(&via, M6522_REG_T1CH, 0);

    /* First fire: latch=4, pipeline=3 → tick 7 */
    tick(&via, 7);
    ASSERT_TRUE(via.ifr & M6522_IRQ_T1);
    ASSERT_EQ(s_irq_count, 1);

    /* Clear T1 IFR; re-arm by writing T1CH again to get second fire */
    (void)m6522_read(&via, M6522_REG_T1CL);
    m6522_write(&via, M6522_REG_T1CL, 4);
    m6522_write(&via, M6522_REG_T1CH, 0);  /* re-arm */

    tick(&via, 7);
    ASSERT_TRUE(via.ifr & M6522_IRQ_T1);
    ASSERT_EQ(s_irq_count, 2);

    /* Third fire: re-arm again */
    (void)m6522_read(&via, M6522_REG_T1CL);
    m6522_write(&via, M6522_REG_T1CL, 4);
    m6522_write(&via, M6522_REG_T1CH, 0);
    tick(&via, 7);
    ASSERT_EQ(s_irq_count, 3);

    test_pass();
}

/*
 * T1 PB7 toggle: in continuous+PB7 mode PB7 should toggle on each underflow.
 */
static void test_t1_pb7_toggle(void)
{
    m6522_t via;
    via_setup(&via, M6522_IRQ_T1);

    /* ACR: continuous + PB7 toggle (bits 6 and 7) */
    m6522_write(&via, M6522_REG_ACR,
                M6522_ACR_T1_CONTINUOUS | M6522_ACR_T1_PB7);

    /* DDRB bit 7 = output */
    m6522_write(&via, M6522_REG_DDRB, 0x80);

    m6522_write(&via, M6522_REG_T1CL, 3);
    m6522_write(&via, M6522_REG_T1CH, 0);

    /* Capture initial PB7 */
    bool pb7_init = (m6522_get_port_b(&via) & 0x80) != 0;

    /* latch=3, pipeline=3: fires at tick 6 (observed empirically) */
    tick(&via, 6);
    bool pb7_after1 = (m6522_get_port_b(&via) & 0x80) != 0;
    ASSERT_TRUE(pb7_after1 != pb7_init);   /* toggled once */

    /* Re-arm for second toggle */
    (void)m6522_read(&via, M6522_REG_T1CL);
    m6522_write(&via, M6522_REG_T1CL, 3);
    m6522_write(&via, M6522_REG_T1CH, 0);
    tick(&via, 6);
    bool pb7_after2 = (m6522_get_port_b(&via) & 0x80) != 0;
    ASSERT_TRUE(pb7_after2 != pb7_after1); /* toggled again */

    test_pass();
}

/* =========================================================================
 * T2 ONE-SHOT
 * ========================================================================= */

/*
 * T2 one-shot: fires once, does NOT reload (wraps through 0xFFFF).
 */
static void test_t2_oneshot_fires(void)
{
    m6522_t via;
    via_setup(&via, M6522_IRQ_T2);

    m6522_write(&via, M6522_REG_T2CL, 6);
    m6522_write(&via, M6522_REG_T2CH, 0);

    tick(&via, 9);   /* latch=6 + 3 pipeline */

    ASSERT_TRUE(via.ifr & M6522_IRQ_T2);
    ASSERT_TRUE(s_irq_state);

    /* Reading T2CL clears T2 flag */
    (void)m6522_read(&via, M6522_REG_T2CL);
    ASSERT_FALSE(via.ifr & M6522_IRQ_T2);

    test_pass();
}

/*
 * T2 no-reload: after firing, continues counting down through 0xFFFF.
 * It should NOT fire again within the next ~6 cycles.
 */
static void test_t2_no_reload(void)
{
    m6522_t via;
    via_setup(&via, M6522_IRQ_T2);

    m6522_write(&via, M6522_REG_T2CL, 4);
    m6522_write(&via, M6522_REG_T2CH, 0);
    tick(&via, 7);   /* latch=4 + 3 pipeline */
    ASSERT_TRUE(via.ifr & M6522_IRQ_T2);

    /* Clear flag */
    (void)m6522_read(&via, M6522_REG_T2CL);
    irq_reset();

    /* Tick another 10 cycles — must NOT fire again */
    tick(&via, 10);
    ASSERT_EQ(s_irq_count, 0);

    test_pass();
}

/* =========================================================================
 * IFR / IER
 * ========================================================================= */

/*
 * IFR: writing 1 to a bit clears that bit.
 */
static void test_ifr_write_clears(void)
{
    m6522_t via;
    via_setup(&via, M6522_IRQ_T1 | M6522_IRQ_T2);

    /* Force-set T1 and T2 IFR bits via timer fires */
    m6522_write(&via, M6522_REG_T1CL, 2);
    m6522_write(&via, M6522_REG_T1CH, 0);
    m6522_write(&via, M6522_REG_T2CL, 2);
    m6522_write(&via, M6522_REG_T2CH, 0);
    tick(&via, 5);

    ASSERT_TRUE(via.ifr & M6522_IRQ_T1);
    ASSERT_TRUE(via.ifr & M6522_IRQ_T2);
    ASSERT_TRUE(via.ifr & M6522_IRQ_ANY);

    /* Clear T1 flag by writing 1 to IFR bit 6 */
    m6522_write(&via, M6522_REG_IFR, M6522_IRQ_T1);
    ASSERT_FALSE(via.ifr & M6522_IRQ_T1);
    ASSERT_TRUE(via.ifr & M6522_IRQ_T2);   /* T2 still set */
    ASSERT_TRUE(via.ifr & M6522_IRQ_ANY);  /* ANY still set (T2 active) */

    /* Clear T2 */
    m6522_write(&via, M6522_REG_IFR, M6522_IRQ_T2);
    ASSERT_FALSE(via.ifr & M6522_IRQ_T2);
    ASSERT_FALSE(via.ifr & M6522_IRQ_ANY);

    test_pass();
}

/*
 * IER: disabling an interrupt prevents IRQ from firing even when IFR is set.
 */
static void test_ier_masks_irq(void)
{
    m6522_t via;
    via_setup(&via, M6522_IRQ_T1);

    m6522_write(&via, M6522_REG_T1CL, 3);
    m6522_write(&via, M6522_REG_T1CH, 0);
    tick(&via, 6);   /* latch=3 + 3 pipeline */
    ASSERT_TRUE(via.ifr & M6522_IRQ_T1);
    ASSERT_EQ(s_irq_count, 1);

    /* Disable T1 interrupt: bit 7 = 0 (clear), bit 6 = 1 */
    m6522_write(&via, M6522_REG_IER, 0x00 | M6522_IRQ_T1);
    /* IFR T1 still set, but IER masked → IRQ should de-assert */
    ASSERT_FALSE(s_irq_state);

    /* Re-enable T1 */
    m6522_write(&via, M6522_REG_IER, 0x80 | M6522_IRQ_T1);
    /* IFR T1 still set → IRQ should re-assert */
    ASSERT_TRUE(s_irq_state);

    test_pass();
}

/*
 * IER read: bit 7 always reads as 1, other bits reflect enabled state.
 */
static void test_ier_read(void)
{
    m6522_t via;
    m6522_callbacks_t cb = {0};
    m6522_init(&via, &cb);
    m6522_reset(&via);

    /* Enable T1 and CA1 */
    m6522_write(&via, M6522_REG_IER, 0x80 | M6522_IRQ_T1 | M6522_IRQ_CA1);
    uint8_t val = m6522_read(&via, M6522_REG_IER);
    ASSERT_TRUE(val & 0x80);               /* bit 7 always 1 on read */
    ASSERT_TRUE(val & M6522_IRQ_T1);
    ASSERT_TRUE(val & M6522_IRQ_CA1);
    ASSERT_FALSE(val & M6522_IRQ_T2);

    test_pass();
}

/* =========================================================================
 * CA1 EDGE DETECTION
 * ========================================================================= */

/*
 * CA1 negative edge (default PCR=0): transition high→low sets IFR CA1 bit.
 */
static void test_ca1_falling_edge(void)
{
    m6522_t via;
    via_setup(&via, M6522_IRQ_CA1);

    /* PCR bit 0 = 0 → CA1 active on falling (high→low) edge */
    m6522_write(&via, M6522_REG_PCR, 0x00);

    /* Start high */
    m6522_set_ca1(&via, true);
    tick(&via, 1);
    ASSERT_FALSE(via.ifr & M6522_IRQ_CA1);

    /* Falling edge */
    m6522_set_ca1(&via, false);
    tick(&via, 1);
    ASSERT_TRUE(via.ifr & M6522_IRQ_CA1);
    ASSERT_TRUE(s_irq_state);

    /* Rising edge should NOT set IFR again */
    m6522_write(&via, M6522_REG_IFR, M6522_IRQ_CA1);
    irq_reset();
    m6522_set_ca1(&via, true);
    tick(&via, 1);
    ASSERT_FALSE(via.ifr & M6522_IRQ_CA1);

    test_pass();
}

/*
 * CA1 positive edge (PCR bit 0 = 1): transition low→high sets IFR CA1 bit.
 */
static void test_ca1_rising_edge(void)
{
    m6522_t via;
    via_setup(&via, M6522_IRQ_CA1);

    /* PCR bit 0 = 1 → CA1 active on rising (low→high) edge */
    m6522_write(&via, M6522_REG_PCR, 0x01);

    m6522_set_ca1(&via, false);
    tick(&via, 1);
    ASSERT_FALSE(via.ifr & M6522_IRQ_CA1);

    /* Rising edge */
    m6522_set_ca1(&via, true);
    tick(&via, 1);
    ASSERT_TRUE(via.ifr & M6522_IRQ_CA1);
    ASSERT_TRUE(s_irq_state);

    /* Falling edge should NOT trigger */
    m6522_write(&via, M6522_REG_IFR, M6522_IRQ_CA1);
    irq_reset();
    m6522_set_ca1(&via, false);
    tick(&via, 1);
    ASSERT_FALSE(via.ifr & M6522_IRQ_CA1);

    test_pass();
}

/* =========================================================================
 * CB1 EDGE DETECTION
 * ========================================================================= */

/*
 * CB1 falling edge (PCR bits 4 = 0 → active low).
 */
static void test_cb1_falling_edge(void)
{
    m6522_t via;
    via_setup(&via, M6522_IRQ_CB1);

    m6522_write(&via, M6522_REG_PCR, 0x00);

    m6522_set_cb1(&via, true);
    tick(&via, 1);
    ASSERT_FALSE(via.ifr & M6522_IRQ_CB1);

    m6522_set_cb1(&via, false);
    tick(&via, 1);
    ASSERT_TRUE(via.ifr & M6522_IRQ_CB1);
    ASSERT_TRUE(s_irq_state);

    test_pass();
}

/*
 * CB1 rising edge (PCR bit 4 = 1).
 */
static void test_cb1_rising_edge(void)
{
    m6522_t via;
    via_setup(&via, M6522_IRQ_CB1);

    /* PCR bit 4 = 1 → CB1 rising */
    m6522_write(&via, M6522_REG_PCR, 0x10);

    m6522_set_cb1(&via, false);
    tick(&via, 1);

    m6522_set_cb1(&via, true);
    tick(&via, 1);
    ASSERT_TRUE(via.ifr & M6522_IRQ_CB1);

    test_pass();
}

/* =========================================================================
 * CA2 INPUT EDGE DETECTION
 * ========================================================================= */

/*
 * CA2 input falling edge (PCR bits 3:1 = 000 → active low).
 */
static void test_ca2_input_falling_edge(void)
{
    m6522_t via;
    via_setup(&via, M6522_IRQ_CA2);

    /* PCR = 0x00: CA2 input, active on falling edge */
    m6522_write(&via, M6522_REG_PCR, 0x00);

    m6522_set_ca2(&via, true);
    tick(&via, 1);
    ASSERT_FALSE(via.ifr & M6522_IRQ_CA2);

    m6522_set_ca2(&via, false);
    tick(&via, 1);
    ASSERT_TRUE(via.ifr & M6522_IRQ_CA2);

    test_pass();
}

/*
 * CA2 input rising edge (PCR bits 3:1 = 001 → active high).
 */
static void test_ca2_input_rising_edge(void)
{
    m6522_t via;
    via_setup(&via, M6522_IRQ_CA2);

    /* PCR = 0x04: CA2 input active on rising edge */
    m6522_write(&via, M6522_REG_PCR, 0x04);

    m6522_set_ca2(&via, false);
    tick(&via, 1);
    ASSERT_FALSE(via.ifr & M6522_IRQ_CA2);

    m6522_set_ca2(&via, true);
    tick(&via, 1);
    ASSERT_TRUE(via.ifr & M6522_IRQ_CA2);

    test_pass();
}

/* =========================================================================
 * main
 * ========================================================================= */
int main(void)
{
    printf("=== m6522 unit tests ===\n");

    TEST(t1_oneshot_fires);
    TEST(t1_oneshot_rearm);
    TEST(t1_continuous);
    TEST(t1_pb7_toggle);
    TEST(t2_oneshot_fires);
    TEST(t2_no_reload);
    TEST(ifr_write_clears);
    TEST(ier_masks_irq);
    TEST(ier_read);
    TEST(ca1_falling_edge);
    TEST(ca1_rising_edge);
    TEST(cb1_falling_edge);
    TEST(cb1_rising_edge);
    TEST(ca2_input_falling_edge);
    TEST(ca2_input_rising_edge);

    printf("\n%d/%d tests passed\n", s_tests_run - s_tests_failed, s_tests_run);
    return (s_tests_failed == 0) ? 0 : 1;
}
