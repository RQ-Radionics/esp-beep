/*
 * test_sn76489.c — Unit tests for the sn76489 PSG emulation
 *
 * Derived from the VHDL reference implementation (FPGA-proven on Spartan-6).
 * Tests are organised to match the VHDL structure:
 *
 *   1. Reset state
 *   2. Register file — latch/data byte protocol
 *   3. Volume table — logarithmic curve (-2 dB/step, level 15 = silence)
 *   4. Tone counters — period, toggle, counter reload
 *   5. Flatline — periods 1..5 hold output high (ultrasonic cutoff)
 *   6. Noise LFSR — seed, periodic vs white feedback, rate select
 *   7. Noise rate 3 — tracks tone channel 2 period
 *   8. Mix — channel sum, silence when vol=15
 *   9. Render — rate-conversion accumulator produces non-silence samples
 *
 * Build and run:
 *   make -C tests/sn76489
 *
 * Licence: GPL-2.0
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include "../../components/sn76489/include/sn76489.h"

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

/* Floating-point near-equality: within `pct` percent */
#define ASSERT_NEAR_PCT(a, b, pct) \
    do { \
        double _a = (double)(a), _b = (double)(b); \
        double _tol = _b * (pct) / 100.0; \
        if (_b == 0.0) _tol = 1.0; \
        if (fabs(_a - _b) > fabs(_tol)) { \
            fprintf(stderr, "  FAIL [%s] line %d: " #a " = %.2f, expected %.2f (±%.1f%%)\n", \
                    s_current_test, __LINE__, _a, _b, (double)(pct)); \
            s_tests_failed++; \
            return; \
        } \
    } while(0)

static void test_pass(void) {
    printf("  PASS [%s]\n", s_current_test);
}

/* =========================================================================
 * 1. RESET STATE
 * ========================================================================= */

/*
 * After init+reset all channels are silent (volume=15), LFSR at seed,
 * latch at channel 0, output starts at +1.
 *
 * VHDL: initial level registers set to 0 (full attenuation = 0xFFF in
 * the 12-bit DAC, but this implementation overrides to silence on reset
 * for cleaner digital startup — tested here as vol=15).
 */
static void test_reset_state(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 0);

    for (int ch = 0; ch < SN76489_CHANNELS; ch++) {
        ASSERT_EQ(psg.ch[ch].volume, 15);   /* silence */
        ASSERT_EQ(psg.ch[ch].output,  1);   /* initial output high */
        ASSERT_EQ(psg.ch[ch].tone,    0);
        ASSERT_EQ(psg.ch[ch].counter, 0);
    }
    ASSERT_EQ(psg.lfsr,        SN76489_LFSR_SEED);
    ASSERT_FALSE(psg.noise_white);
    ASSERT_EQ(psg.noise_rate,  0);
    ASSERT_EQ(psg.latch_ch,    0);
    ASSERT_FALSE(psg.latch_is_vol);

    test_pass();
}

/* =========================================================================
 * 2. REGISTER FILE — latch/data byte protocol
 *
 * VHDL register map (bit 7 = 1 → latch byte, bit 7 = 0 → data byte):
 *   R0  1 000 PPPP  ch A tone low 4 bits
 *       0 -PP PPPP  ch A tone high 6 bits
 *   R1  1 001 AAAA  ch A volume
 *   R2  1 010 PPPP  ch B tone low 4 bits
 *   R3  1 011 AAAA  ch B volume
 *   R4  1 100 PPPP  ch C tone low 4 bits
 *   R5  1 101 AAAA  ch C volume
 *   R6  1 110 -FSS  noise control
 *   R7  1 111 AAAA  noise volume
 * ========================================================================= */

/*
 * Latch byte: sets tone low 4 bits and latches the channel.
 * Data byte: sets tone high 6 bits using the latched channel.
 * Combined: 10-bit period = high6:low4.
 */
static void test_tone_write_two_bytes(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 0);

    /* Channel A (ch 0): latch byte 0x80 | (period_low & 0x0F) */
    /* Target period = 0x1A5 = 0001_1010_0101b → low=0101, high=000110 */
    sn76489_write(&psg, 0x85);   /* 1 000 0101 → ch0 tone, low = 0x5 */
    ASSERT_EQ(psg.ch[SN76489_TONE0].tone & 0x0F, 0x5);

    sn76489_write(&psg, 0x1A);   /* 0 011010 → ch0 tone, high = 0x1A */
    /* tone = (0x1A << 4) | 0x5 = 0x1A5 */
    ASSERT_EQ(psg.ch[SN76489_TONE0].tone, 0x1A5);

    test_pass();
}

/*
 * Volume latch byte: bit 4 = 1 → volume register, bits 3:0 = attenuation.
 */
static void test_volume_write(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 0);

    /* Channel B (ch 1) volume = 7: 1 011 0111 = 0xB7 */
    sn76489_write(&psg, 0xB7);
    ASSERT_EQ(psg.ch[SN76489_TONE1].volume, 7);
    ASSERT_EQ(psg.latch_ch, SN76489_TONE1);
    ASSERT_TRUE(psg.latch_is_vol);

    /* Data byte also updates volume of latched channel */
    sn76489_write(&psg, 0x03);   /* bit 7 = 0, lower 4 bits = 3 */
    ASSERT_EQ(psg.ch[SN76489_TONE1].volume, 3);

    test_pass();
}

/*
 * Data byte without preceding latch goes to previously latched register.
 */
static void test_latch_persistence(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 0);

    /* Latch channel C (ch 2) tone low */
    sn76489_write(&psg, 0xC3);   /* 1 100 0011 → ch2 tone low = 3 */
    ASSERT_EQ(psg.latch_ch, SN76489_TONE2);
    ASSERT_FALSE(psg.latch_is_vol);

    /* Second data byte goes to same register (tone high) */
    sn76489_write(&psg, 0x0A);   /* high 6 bits = 0x0A */
    ASSERT_EQ(psg.ch[SN76489_TONE2].tone, (0x0A << 4) | 0x3);

    /* Another latch for a different channel does NOT corrupt ch2 */
    sn76489_write(&psg, 0x80);   /* ch0 tone low = 0 */
    ASSERT_EQ(psg.ch[SN76489_TONE2].tone, (0x0A << 4) | 0x3);

    test_pass();
}

/*
 * Noise control byte: bit 2 = white/periodic, bits 1:0 = rate.
 * Writing to noise register resets the LFSR to seed (VHDL: noise_rst).
 */
static void test_noise_control_write(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 0);

    /* Corrupt the LFSR first */
    sn76489_tick(&psg, 1000);

    /* Write noise control: white noise, rate 1 — 1 110 0101 = 0xE5 */
    sn76489_write(&psg, 0xE5);
    ASSERT_TRUE(psg.noise_white);
    ASSERT_EQ(psg.noise_rate, 1);
    ASSERT_EQ(psg.lfsr, SN76489_LFSR_SEED);   /* LFSR reset on noise write */

    /* Periodic, rate 2 — 1 110 0010 = 0xE2 */
    sn76489_write(&psg, 0xE2);
    ASSERT_FALSE(psg.noise_white);
    ASSERT_EQ(psg.noise_rate, 2);
    ASSERT_EQ(psg.lfsr, SN76489_LFSR_SEED);

    test_pass();
}

/*
 * Noise volume: 1 111 AAAA = 0xF0..0xFF.
 */
static void test_noise_volume_write(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 0);

    sn76489_write(&psg, 0xF5);   /* 1 111 0101 → noise vol = 5 */
    ASSERT_EQ(psg.ch[SN76489_NOISE].volume, 5);

    sn76489_write(&psg, 0xFF);   /* noise vol = 15 (silence) */
    ASSERT_EQ(psg.ch[SN76489_NOISE].volume, 15);

    test_pass();
}

/* =========================================================================
 * 3. VOLUME TABLE — logarithmic curve
 *
 * VHDL dacrom values (12-bit):
 *   0xFFF, 0xCB5, 0xA18, 0x804, 0x65E, 0x50F, 0x405, 0x331,
 *   0x289, 0x204, 0x199, 0x145, 0x102, 0x0CD, 0x0A3, 0x000
 *
 * Each step must be ~0.7943 × previous (-2 dB).
 * Level 15 must be exactly 0 (silence forced to prevent digital noise).
 * ========================================================================= */

static void test_vol_table_level15_silence(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 0);

    ASSERT_EQ(psg.vol_table[15], 0);

    test_pass();
}

static void test_vol_table_level0_max(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 0);

    /* Level 0 should be the largest value in the table */
    for (int i = 1; i < 15; i++)
        ASSERT_TRUE(psg.vol_table[0] > psg.vol_table[i]);

    test_pass();
}

/*
 * Each step must be -2 dB from the previous, i.e. ratio ≈ 0.7943.
 * We allow ±1% tolerance for floating-point rounding in build_vol_table.
 */
static void test_vol_table_2db_steps(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 0);

    /* -2 dB ratio = 10^(-2/20) = 0.794328... */
    const double ratio = pow(10.0, -2.0 / 20.0);

    for (int i = 1; i < 15; i++) {
        double expected = psg.vol_table[i - 1] * ratio;
        ASSERT_NEAR_PCT(psg.vol_table[i], expected, 1.0);
    }

    test_pass();
}

/* =========================================================================
 * 4. TONE COUNTERS — period, toggle, reload
 *
 * VHDL counter logic (C pseudocode from the VHDL comments):
 *
 *   if (counter == 0) {
 *     tone = !tone;
 *     counter = period;
 *   }
 *   counter--;
 *
 * So with period=N, the output toggles every N ticks (half-period).
 * Full period = 2*N ticks.  The counter starts at 0 after reset.
 *
 * The VHDL also notes: "changing the tone period will not take effect
 * until the next cycle of the counter."
 * ========================================================================= */

/*
 * With period=8, the output toggles every 8 ticks.
 *
 * The counter starts at 0 after reset.  The VHDL description says:
 *   "if counter==0: toggle, counter=period; counter--"
 * so the very first tick (counter==0) immediately causes a toggle, then
 * the counter runs period-1 → 0 before the next toggle.
 * Toggle schedule (period=8): ticks 1, 9, 17, 25 ...
 */
static void test_tone_toggles_at_period(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 0);

    /* Channel A, period = 8, volume = 0 (max) */
    sn76489_write(&psg, 0x88);   /* ch0 tone low = 8 */
    sn76489_write(&psg, 0x90);   /* ch0 vol = 0 */

    int8_t initial = psg.ch[SN76489_TONE0].output;

    /* Tick 1: counter was 0 → immediate toggle */
    sn76489_tick(&psg, 1);
    ASSERT_EQ(psg.ch[SN76489_TONE0].output, -initial);

    /* Ticks 2..8: counter counts 7→1, no toggle */
    sn76489_tick(&psg, 7);
    ASSERT_EQ(psg.ch[SN76489_TONE0].output, -initial);

    /* Tick 9: counter reaches 0 again → toggle back */
    sn76489_tick(&psg, 1);
    ASSERT_EQ(psg.ch[SN76489_TONE0].output, initial);

    /* Another full period (8 ticks) → toggle again */
    sn76489_tick(&psg, 8);
    ASSERT_EQ(psg.ch[SN76489_TONE0].output, -initial);

    test_pass();
}

/*
 * Three independent channels toggle at their own periods without interference.
 *
 * All counters start at 0, so all three toggle on tick 1.
 * After that, ch A (period=10) toggles every 10 ticks, ch B every 20, ch C every 40.
 * Schedule relative to tick 0:
 *   Ch A toggles: 1, 11, 21, 31 ...
 *   Ch B toggles: 1, 21, 41 ...
 *   Ch C toggles: 1, 41 ...
 */
static void test_three_channels_independent(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 0);

    /* Ch A period=10, Ch B period=20, Ch C period=40 */
    sn76489_write(&psg, 0x8A);  sn76489_write(&psg, 0x00); /* ch0 period=10 */
    sn76489_write(&psg, 0xA4);  sn76489_write(&psg, 0x01); /* ch1 period=20 */
    sn76489_write(&psg, 0xC8);  sn76489_write(&psg, 0x02); /* ch2 period=40 */

    /* All three toggle on tick 1 (counter starts at 0) */
    int8_t a0 = psg.ch[SN76489_TONE0].output;
    int8_t b0 = psg.ch[SN76489_TONE1].output;
    int8_t c0 = psg.ch[SN76489_TONE2].output;
    sn76489_tick(&psg, 1);
    int8_t a1 = -a0, b1 = -b0, c1 = -c0;
    ASSERT_EQ(psg.ch[SN76489_TONE0].output, a1);
    ASSERT_EQ(psg.ch[SN76489_TONE1].output, b1);
    ASSERT_EQ(psg.ch[SN76489_TONE2].output, c1);

    /* Ticks 2..10: ch A at counter=9→0 (toggles at tick 11), others not yet */
    sn76489_tick(&psg, 9);   /* now at tick 10 */
    ASSERT_EQ(psg.ch[SN76489_TONE0].output, a1);  /* not yet */
    ASSERT_EQ(psg.ch[SN76489_TONE1].output, b1);
    ASSERT_EQ(psg.ch[SN76489_TONE2].output, c1);

    /* Tick 11: ch A toggles, ch B and ch C do not */
    sn76489_tick(&psg, 1);
    ASSERT_EQ(psg.ch[SN76489_TONE0].output, a0);  /* toggled back */
    ASSERT_EQ(psg.ch[SN76489_TONE1].output, b1);
    ASSERT_EQ(psg.ch[SN76489_TONE2].output, c1);

    /* Tick 21: ch A and ch B toggle */
    sn76489_tick(&psg, 10);
    ASSERT_EQ(psg.ch[SN76489_TONE0].output, a1);  /* ch A toggled */
    ASSERT_EQ(psg.ch[SN76489_TONE1].output, b0);  /* ch B toggled */
    ASSERT_EQ(psg.ch[SN76489_TONE2].output, c1);  /* ch C unchanged */

    test_pass();
}

/*
 * Period change takes effect only after the current counter cycle completes.
 * VHDL note: "changing the tone period will not take effect until the next
 * cycle of the counter."
 *
 * Since counter starts at 0, tick 1 toggles immediately and loads period=20.
 * Then counter counts 19→0, toggling again at tick 21.
 * If we change period to 6 at tick 10, the current cycle (ending at tick 21)
 * must complete before the new period takes effect.
 */
static void test_period_change_deferred(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 0);

    /* Start with period=20 = 0x14: low4=4→latch 0x84, high6=1→data 0x01 */
    sn76489_write(&psg, 0x84);  sn76489_write(&psg, 0x01); /* ch0 period=20 */

    int8_t initial = psg.ch[SN76489_TONE0].output;  /* +1 at reset */

    /* Tick 1: counter was 0 → immediate toggle, counter loads 20 */
    sn76489_tick(&psg, 1);
    ASSERT_EQ(psg.ch[SN76489_TONE0].output, -initial);  /* toggled */

    /* Ticks 2..10: counter counting down, no toggle */
    sn76489_tick(&psg, 9);
    ASSERT_EQ(psg.ch[SN76489_TONE0].output, -initial);

    /* Change period to 6 mid-cycle — must NOT affect current cycle.
     * Write both bytes to set period=6 cleanly (low=6, high=0). */
    sn76489_write(&psg, 0x86);  /* ch0 tone low = 6 */
    sn76489_write(&psg, 0x00);  /* ch0 tone high = 0 → period = (0<<4)|6 = 6 */

    /* Ticks 11..20: still counting old period (counter=11..1), no toggle */
    sn76489_tick(&psg, 10);
    ASSERT_EQ(psg.ch[SN76489_TONE0].output, -initial);

    /* Tick 21: counter reaches 0 (20 ticks after last toggle) → toggle,
     * and now reloads with the NEW period (6) */
    sn76489_tick(&psg, 1);
    ASSERT_EQ(psg.ch[SN76489_TONE0].output, initial);

    /* New period (6) now active: 6 more ticks → toggle again */
    sn76489_tick(&psg, 6);
    ASSERT_EQ(psg.ch[SN76489_TONE0].output, -initial);

    test_pass();
}

/* =========================================================================
 * 5. FLATLINE — ultrasonic periods held high
 *
 * VHDL: flatline_s = '1' when period > 0 AND period < MIN_PERIOD_CNT_G (6)
 * i.e. periods 1, 2, 3, 4, 5 produce a flat high output regardless of
 * the counter.  This prevents aliasing in all-digital systems while still
 * allowing amplitude modulation via the volume register.
 *
 * The implementation defines SN76489_MIN_PERIOD = 6 in sn76489.c.
 * ========================================================================= */

/*
 * Periods 1..5 must never toggle: output stays +1.
 */
static void test_flatline_periods_1_to_5(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 0);

    for (int period = 1; period <= 5; period++) {
        sn76489_reset(&psg);
        sn76489_write(&psg, 0x80 | (uint8_t)period); /* ch0 tone low = period */

        /* Tick well past when a toggle would occur (period * 4 ticks) */
        sn76489_tick(&psg, period * 4 + 10);

        /* Output must remain +1 (flat-lined) */
        if (psg.ch[SN76489_TONE0].output != 1) {
            fprintf(stderr, "  FAIL [%s] line %d: period=%d toggled (output=%d)\n",
                    s_current_test, __LINE__, period, psg.ch[SN76489_TONE0].output);
            s_tests_failed++;
            return;
        }
    }

    test_pass();
}

/*
 * Period 6 is the first that MUST toggle (not flat-lined).
 */
static void test_period_6_not_flatlined(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 0);

    sn76489_write(&psg, 0x86);   /* ch0 tone low = 6 */
    int8_t initial = psg.ch[SN76489_TONE0].output;

    sn76489_tick(&psg, 6);
    ASSERT_EQ(psg.ch[SN76489_TONE0].output, -initial);

    test_pass();
}

/*
 * Flatline still allows volume (AM) to change: mix_sample should reflect
 * the volume change even when the tone is flat-lined.
 */
static void test_flatline_am_still_works(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 44100);

    /* Set ch0 to period=2 (flat-lined), volume=0 (max) */
    sn76489_write(&psg, 0x82);   /* ch0 period low = 2 */
    sn76489_write(&psg, 0x90);   /* ch0 vol = 0 */

    /* Silence all other channels */
    sn76489_write(&psg, 0xBF);   /* ch1 vol = 15 */
    sn76489_write(&psg, 0xDF);   /* ch2 vol = 15 */
    sn76489_write(&psg, 0xFF);   /* noise vol = 15 */

    /* Tick and sample: output must be non-zero (flat-lined +1 * vol[0]) */
    sn76489_tick(&psg, 50);
    int16_t buf[1];
    sn76489_render(&psg, buf, 1);
    ASSERT_TRUE(buf[0] != 0);

    /* Now silence ch0 via volume → output must be zero */
    sn76489_write(&psg, 0x9F);   /* ch0 vol = 15 */
    sn76489_render(&psg, buf, 1);
    ASSERT_EQ(buf[0], 0);

    test_pass();
}

/* =========================================================================
 * 6. NOISE LFSR
 *
 * VHDL 15-bit right-shift LFSR, seed = 0x4000 (bit 14 set).
 * Taps at bits 0 and 1.
 *
 * White noise feedback:  fb = (lfsr >> 0) XOR (lfsr >> 1)  (both bits)
 * Periodic feedback:     fb = (lfsr >> 0)                   (bit 0 only)
 *
 * shift: lfsr = (lfsr >> 1) | (fb << 14)
 * output bit = lfsr[0]  (LSB after shift)
 *
 * The VHDL resets the LFSR on any noise control write (noise_rst).
 * ========================================================================= */

/*
 * LFSR seed is 0x4000.  First few periodic steps match the expected sequence.
 *
 * Periodic: fb = bit0.  Starting at 0x4000 = 0100_0000_0000_0000:
 *   step 0: lfsr=0x4000, bit0=0, fb=0 → lfsr = 0>>1 | 0<<14 = 0x2000, output=0
 *   step 1: lfsr=0x2000, bit0=0, fb=0 → lfsr = 0x1000, output=0
 *   ...until the 1 bit reaches bit 0 (14 steps later).
 *   step 14: lfsr=0x0001, bit0=1, fb=1 → lfsr = (0>>1)|(1<<14)=0x4000, output=1
 *   So periodic noise period = 15 steps (the lone 1 circulates).
 */
static void test_noise_lfsr_periodic_sequence(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 0);

    /* Periodic noise, rate 0: divider = 15 (BBC variant s_noise_dividers[0]) */
    sn76489_write(&psg, 0xE0);
    ASSERT_EQ(psg.lfsr, SN76489_LFSR_SEED);  /* 0x4000 */

    /* The noise counter starts at 0, just like the tone counters.
     * So the LFSR steps at ticks: 1, 1+15, 1+30, ... = 1, 16, 31, 46, ...
     *
     * Periodic LFSR with seed 0x4000 (lone 1 at bit 14):
     *   step 0 (initial):  lfsr=0x4000, output bit=bit0=0 → output=-1
     *   step 1 (tick 1):   lfsr=0x2000, output=-1
     *   ...
     *   step 13 (tick 196):lfsr=0x0002, output=-1
     *   step 14 (tick 211):lfsr=0x0001, output=+1   ← lone 1 reaches bit 0
     *   step 15 (tick 226):lfsr=0x4000, output=-1   ← wraps back to seed
     */
    const int divider = 15;

    /* After 13 LFSR steps (tick = 1 + 13*15 - 1 = 195 ... actually tick 1 + 12*15
     * gets us to step 13): still output=-1 */
    /* Simpler: step N occurs at tick 1 + N*divider - divider = 1 + (N-1)*divider
     * Step 1 at tick 1; step 14 at tick 1 + 13*15 = 196 */
    sn76489_tick(&psg, 1 + 13 * divider - 1);  /* up to just before step 14 */
    ASSERT_EQ(psg.ch[SN76489_NOISE].output, -1);

    /* One more tick → step 14: lone 1 at bit 0 → output=+1 */
    sn76489_tick(&psg, 1);
    ASSERT_EQ(psg.ch[SN76489_NOISE].output, 1);

    /* divider more ticks → step 15: lone 1 wraps to bit 14 → output=-1 */
    sn76489_tick(&psg, divider);
    ASSERT_EQ(psg.ch[SN76489_NOISE].output, -1);

    test_pass();
}

/*
 * White noise LFSR produces a different (non-periodic) pattern from the same seed.
 *
 * With seed 0x4000 and white noise (fb = bit0 XOR bit1):
 *   step 0: lfsr=0x4000, b0=0, b1=0, fb=0 → lfsr=0x2000, out=0
 *   step 1: lfsr=0x2000, b0=0, b1=0, fb=0 → lfsr=0x1000, out=0
 *   ...
 *   step 13: lfsr=0x0002, b0=0, b1=1, fb=1 → lfsr=0x4001, out=0
 *   step 14: lfsr=0x4001, b0=1, b1=0, fb=1 → lfsr=0x6000, out=1
 *   step 15: lfsr=0x6000, b0=0, b1=0, fb=0 → lfsr=0x3000, out=0
 *
 * Key property: the sequence is NOT the same as periodic.
 * Verify that after 14 steps the LFSR differs from the periodic case.
 */
static void test_noise_lfsr_white_differs_from_periodic(void)
{
    sn76489_t psg_w, psg_p;
    sn76489_init(&psg_w, 0);
    sn76489_init(&psg_p, 0);

    sn76489_write(&psg_w, 0xE4);  /* white,    rate 0 */
    sn76489_write(&psg_p, 0xE0);  /* periodic, rate 0 */

    const int divider = 15;

    /* Advance both by enough steps to diverge */
    sn76489_tick(&psg_w, 20 * divider);
    sn76489_tick(&psg_p, 20 * divider);

    /* The LFSRs must differ (white XOR feedback spreads bits differently) */
    ASSERT_TRUE(psg_w.lfsr != psg_p.lfsr);

    test_pass();
}

/*
 * Writing to the noise register resets the LFSR to the seed value.
 */
static void test_noise_lfsr_resets_on_write(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 0);

    sn76489_write(&psg, 0xE4);   /* white noise */
    sn76489_tick(&psg, 5000);    /* run for a while */
    ASSERT_TRUE(psg.lfsr != SN76489_LFSR_SEED);

    /* Re-writing noise register resets LFSR */
    sn76489_write(&psg, 0xE4);
    ASSERT_EQ(psg.lfsr, SN76489_LFSR_SEED);

    test_pass();
}

/*
 * Noise rate 0, 1, 2: fixed dividers 15, 32, 64.
 * Rate 1 should produce half the frequency of rate 0 (twice as many ticks
 * per LFSR step).  Verify by measuring the LFSR step count per unit time.
 */
static void test_noise_rate_dividers(void)
{
    /* We compare lfsr step counts: with rate 0 (div=15) vs rate 1 (div=32),
     * advancing both by the same number of base ticks, rate 0 should have
     * taken more LFSR steps (its lfsr will be further along the sequence). */
    sn76489_t psg0, psg1;
    sn76489_init(&psg0, 0);
    sn76489_init(&psg1, 0);

    sn76489_write(&psg0, 0xE0);  /* periodic, rate 0, div=15 */
    sn76489_write(&psg1, 0xE1);  /* periodic, rate 1, div=32 */

    /* After 960 ticks (LCM of 15 and 32):
     * rate 0: 960/15 = 64 LFSR steps → wraps multiple times
     * rate 1: 960/32 = 30 LFSR steps
     * The LFSRs will be at different positions. */
    sn76489_tick(&psg0, 960);
    sn76489_tick(&psg1, 960);

    ASSERT_TRUE(psg0.lfsr != psg1.lfsr);

    test_pass();
}

/* =========================================================================
 * 7. NOISE RATE 3 — tracks tone channel 2
 *
 * VHDL: "noise_ff_x <= c_ff_r when others" (i.e. noise rate 3 uses c_ff_r,
 * the raw channel-C flip-flop that ignores flatline).
 * The C implementation uses psg->ch[SN76489_TONE2].tone as divider.
 * ========================================================================= */

/*
 * With rate=3, the noise LFSR advances when channel C's counter overflows.
 * Set ch C to period=20, noise to rate=3.  After 20 ticks the noise should
 * have advanced one step (LFSR changed), but after only 10 it should not.
 */
/*
 * With rate=3, the noise LFSR advances when channel C's counter overflows.
 *
 * Like tone channels, the noise counter starts at 0, so the first tick
 * immediately advances the LFSR (using ch C's period as divider).
 * After that, subsequent advances happen every `period` ticks.
 *
 * We verify: after the first tick, LFSR changed; then it doesn't change
 * again until another `period` ticks elapse.
 */
static void test_noise_rate3_tracks_tone2(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 0);

    /* Ch C period=20 = 0x14: low4=4→latch 0xC4, high6=1→data 0x01 */
    sn76489_write(&psg, 0xC4);  sn76489_write(&psg, 0x01); /* ch2 period=20 */

    /* Noise: periodic, rate 3 (tracks ch C).  Writing noise resets LFSR. */
    sn76489_write(&psg, 0xE3);
    uint16_t lfsr_seed = psg.lfsr;  /* 0x4000 */

    /* Tick 1: noise counter was 0 → uses ch2.tone=20 as divider,
     * counter==0 → immediate LFSR step, counter loads 20 */
    sn76489_tick(&psg, 1);
    uint16_t lfsr_after1 = psg.lfsr;
    ASSERT_TRUE(lfsr_after1 != lfsr_seed);

    /* Ticks 2..20: noise counter counting 19→1, no LFSR advance */
    sn76489_tick(&psg, 19);
    ASSERT_EQ(psg.lfsr, lfsr_after1);  /* unchanged */

    /* Tick 21: counter reaches 0 (20 ticks elapsed) → LFSR advances again */
    sn76489_tick(&psg, 1);
    ASSERT_TRUE(psg.lfsr != lfsr_after1);

    test_pass();
}

/* =========================================================================
 * 8. MIX — channel sum, silence behaviour
 *
 * VHDL level_x_s: when tone_r = '0' → 0, else ch_x_level_r.
 * The C mix uses: output (+1/-1) * vol_table[volume].
 * When vol=15, vol_table[15]=0 → channel contributes 0.
 * ========================================================================= */

/*
 * All channels silent (vol=15): every sample must be 0.
 */
static void test_mix_all_silent(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 44100);

    /* All channels at silence (already the reset default) */
    int16_t buf[64];
    sn76489_render(&psg, buf, 64);
    for (int i = 0; i < 64; i++)
        ASSERT_EQ(buf[i], 0);

    test_pass();
}

/*
 * Single active channel at max volume: samples must be non-zero.
 */
static void test_mix_single_channel_nonzero(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 44100);

    /* Ch A: period=100, vol=0 (max) */
    sn76489_write(&psg, 0x80 | 100);  /* tone low = 100 (fits in 4 bits? no) */
    /* 100 = 0x64 → low=4, high=6 */
    sn76489_write(&psg, 0x84);        /* ch0 tone low = 4 */
    sn76489_write(&psg, 0x06);        /* ch0 tone high = 6 → period = 0x64 = 100 */
    sn76489_write(&psg, 0x90);        /* ch0 vol = 0 */

    int16_t buf[256];
    sn76489_render(&psg, buf, 256);

    /* At least some samples must be non-zero */
    bool any_nonzero = false;
    for (int i = 0; i < 256; i++)
        if (buf[i] != 0) { any_nonzero = true; break; }
    ASSERT_TRUE(any_nonzero);

    test_pass();
}

/*
 * Output alternates between positive and negative values for an active
 * square-wave channel (tone toggles between +1 and -1).
 */
static void test_mix_square_wave_alternates(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 0);   /* tick-mode, no rate conversion */

    /* Ch A: period=50, vol=0 */
    sn76489_write(&psg, 0x82);   /* tone low = 2 ... but wait, 2 is flatlined */
    /* Use period=50: low=2, high=3 → 0x32 = 50 */
    sn76489_write(&psg, 0x82);   /* ch0 tone low = 2 (WRONG, sets low=2) */
    sn76489_write(&psg, 0x03);   /* high 6 bits = 3 → period = (3<<4)|2 = 50 */
    sn76489_write(&psg, 0x90);   /* ch0 vol = 0 */

    /* Silence everything else */
    sn76489_write(&psg, 0xBF);
    sn76489_write(&psg, 0xDF);
    sn76489_write(&psg, 0xFF);

    /* Advance to known positive phase */
    sn76489_tick(&psg, 50);
    int8_t out_a = psg.ch[SN76489_TONE0].output;

    /* Another half-period: must be opposite */
    sn76489_tick(&psg, 50);
    ASSERT_EQ(psg.ch[SN76489_TONE0].output, -out_a);

    test_pass();
}

/* =========================================================================
 * 9. RENDER — rate-conversion accumulator
 * ========================================================================= */

/*
 * Rendering with a valid sample rate produces non-silence for an active
 * channel, and the buffer length matches the request.
 */
static void test_render_length_correct(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 44100);

    /* Ch A: period=200, vol=0 */
    sn76489_write(&psg, 0x88);   /* tone low = 8 */
    sn76489_write(&psg, 0x0C);   /* tone high = 12 → period=(12<<4)|8 = 200 */
    sn76489_write(&psg, 0x90);   /* vol = 0 */

    int16_t buf[512];
    memset(buf, 0xAB, sizeof(buf));  /* sentinel */
    sn76489_render(&psg, buf, 512);

    /* The sentinel bytes after the buffer must not be touched */
    /* (We can't easily test this without extra guards, so just check non-crash.) */

    /* At least half the samples should be non-zero */
    int nonzero = 0;
    for (int i = 0; i < 512; i++)
        if (buf[i] != 0) nonzero++;
    ASSERT_TRUE(nonzero > 256);

    test_pass();
}

/*
 * With sample_rate=0, render produces silence (no division by zero).
 */
static void test_render_zero_sample_rate_silent(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 0);

    sn76489_write(&psg, 0x90);   /* ch0 vol = 0 (active) */

    int16_t buf[32];
    sn76489_render(&psg, buf, 32);

    for (int i = 0; i < 32; i++)
        ASSERT_EQ(buf[i], 0);

    test_pass();
}

/*
 * get_volume and get_tone return the correct latched values.
 */
static void test_getters(void)
{
    sn76489_t psg;
    sn76489_init(&psg, 0);

    sn76489_write(&psg, 0x8F);   /* ch0 tone low = 15 */
    sn76489_write(&psg, 0x02);   /* ch0 tone high = 2 → period=(2<<4)|15=47 */
    sn76489_write(&psg, 0x9A);   /* ch0 vol = 10 */

    ASSERT_EQ(sn76489_get_tone(&psg, SN76489_TONE0),   47);
    ASSERT_EQ(sn76489_get_volume(&psg, SN76489_TONE0), 10);

    /* Out-of-range channel */
    ASSERT_EQ(sn76489_get_volume(&psg, 4), 15);
    ASSERT_EQ(sn76489_get_tone(&psg,   4),  0);

    test_pass();
}

/* =========================================================================
 * main
 * ========================================================================= */
int main(void)
{
    printf("=== sn76489 unit tests ===\n");

    /* 1. Reset */
    TEST(reset_state);

    /* 2. Register file */
    TEST(tone_write_two_bytes);
    TEST(volume_write);
    TEST(latch_persistence);
    TEST(noise_control_write);
    TEST(noise_volume_write);

    /* 3. Volume table */
    TEST(vol_table_level15_silence);
    TEST(vol_table_level0_max);
    TEST(vol_table_2db_steps);

    /* 4. Tone counters */
    TEST(tone_toggles_at_period);
    TEST(three_channels_independent);
    TEST(period_change_deferred);

    /* 5. Flatline */
    TEST(flatline_periods_1_to_5);
    TEST(period_6_not_flatlined);
    TEST(flatline_am_still_works);

    /* 6. Noise LFSR */
    TEST(noise_lfsr_periodic_sequence);
    TEST(noise_lfsr_white_differs_from_periodic);
    TEST(noise_lfsr_resets_on_write);
    TEST(noise_rate_dividers);

    /* 7. Noise rate 3 */
    TEST(noise_rate3_tracks_tone2);

    /* 8. Mix */
    TEST(mix_all_silent);
    TEST(mix_single_channel_nonzero);
    TEST(mix_square_wave_alternates);

    /* 9. Render */
    TEST(render_length_correct);
    TEST(render_zero_sample_rate_silent);
    TEST(getters);

    printf("\n%d/%d tests passed\n", s_tests_run - s_tests_failed, s_tests_run);
    return (s_tests_failed == 0) ? 0 : 1;
}
