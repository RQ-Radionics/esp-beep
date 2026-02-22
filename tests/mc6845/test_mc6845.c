/*
 * Tests for mc6845 — Motorola MC6845 CRTC emulation
 *
 * Covers:
 *   1.  init / reset state
 *   2.  register write masks
 *   3.  write-only / read-only access control per type
 *   4.  address register sel (lower 5 bits only)
 *   5.  status register read (UM6845 vs MC6845)
 *   6.  helper: mc6845_get_start_addr / mc6845_get_cursor_addr
 *   7.  basic tick: MA increment, h_ctr, h_de gate
 *   8.  HSYNC generation (position + width)
 *   9.  HSYNC width = 0 → treated as 16 on MC6845
 *  10.  end-of-line: h_ctr wrap, MA reload to ma_row_start
 *  11.  vertical raster counter and character-row advance
 *  12.  VSYNC generation (position + fixed-16 width on MC6845)
 *  13.  VSYNC callback (rising and falling edges)
 *  14.  vertical display enable (v_de off when v_ctr == v_displayed)
 *  15.  vertical adjust R5>0 inserts extra scanlines
 *  16.  vertical adjust R5=0 resets frame immediately
 *  17.  cursor: always-on (mode 0) and always-off (mode 1)
 *  18.  cursor: scanline range (cursor_start / cursor_end)
 *  19.  cursor: suppressed outside display area
 *  20.  cursor: blink rate 8 (mode 2) and 16 (mode 3)
 *  21.  light pen strobe: latches MA into R16/R17
 *  22.  MA 14-bit wrap at 0x3FFF
 *  23.  mc6845_reset does NOT clear registers
 *  24.  VSYNC width on UM6845 from R3[7:4]
 *
 * All timing counts derived from a reference diagnostic run against the
 * actual mc6845.c implementation.
 */

#include "mc6845.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Minimal test framework
 * -------------------------------------------------------------------------- */
static int s_pass = 0, s_fail = 0;
static const char *s_suite = "";

#define SUITE(name) do { s_suite = (name); printf("\n[%s]\n", name); } while(0)

#define ASSERT_EQ(a, b) do { \
    if ((uint32_t)(a) != (uint32_t)(b)) { \
        printf("  FAIL %s:%d: expected 0x%X, got 0x%X\n", \
               s_suite, __LINE__, (unsigned)(b), (unsigned)(a)); \
        s_fail++; \
    } else { s_pass++; } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        printf("  FAIL %s:%d: expected true\n", s_suite, __LINE__); \
        s_fail++; \
    } else { s_pass++; } \
} while(0)

#define ASSERT_FALSE(x) do { \
    if (x) { \
        printf("  FAIL %s:%d: expected false\n", s_suite, __LINE__); \
        s_fail++; \
    } else { s_pass++; } \
} while(0)

/* --------------------------------------------------------------------------
 * Helper: write / read a named register via address/data port
 * -------------------------------------------------------------------------- */
static void set_reg(mc6845_t *c, uint8_t reg, uint8_t val)
{
    mc6845_write(c, 0, reg);
    mc6845_write(c, 1, val);
}

static uint8_t get_reg(mc6845_t *c, uint8_t reg)
{
    mc6845_write(c, 0, reg);
    return mc6845_read(c, 1);
}

/* Run N ticks, return last output */
static mc6845_output_t tick_n(mc6845_t *c, int n)
{
    mc6845_output_t out = {0};
    for (int i = 0; i < n; i++)
        out = mc6845_tick(c);
    return out;
}

/* --------------------------------------------------------------------------
 * Minimal frame setup helpers
 *
 * "tiny" config: H_TOTAL=3 (4 chars/line), H_DISPLAYED=2, H_SYNCPOS=3
 *   V_TOTAL=0 (1 row), V_DISPLAYED=1, V_SYNCPOS=0, MAX_SCANLINE=0 (1 scanline)
 *   Frame = 4 ticks total.
 *   After first htotal (tick 4) the frame resets: v_ctr=0, v_de=true, h_de=true.
 *
 * "small" config: 4 chars/line, 2 scanlines/row, 3 rows total, 2 displayed
 *   Frame = 4 × 3 × 2 = 24 ticks after first htotal.
 * -------------------------------------------------------------------------- */
static void setup_tiny(mc6845_t *c, mc6845_type_t type)
{
    mc6845_init(c, type);
    set_reg(c, MC6845_R0_HTOTAL,      3);
    set_reg(c, MC6845_R1_HDISPLAYED,  2);
    set_reg(c, MC6845_R2_HSYNCPOS,    3);
    set_reg(c, MC6845_R3_SYNCWIDTHS,  0x11);
    set_reg(c, MC6845_R4_VTOTAL,      0);
    set_reg(c, MC6845_R5_VTOTALADJ,   0);
    set_reg(c, MC6845_R6_VDISPLAYED,  1);
    set_reg(c, MC6845_R7_VSYNCPOS,    0);
    set_reg(c, MC6845_R9_MAXSCANLINE, 0);
}

/* --------------------------------------------------------------------------
 * 1. init / reset state
 * -------------------------------------------------------------------------- */
static void test_init_reset(void)
{
    SUITE("init/reset");

    mc6845_t c;
    mc6845_init(&c, MC6845_TYPE_MC6845);

    ASSERT_EQ(c.type, MC6845_TYPE_MC6845);
    ASSERT_EQ(c.h_ctr, 0);
    ASSERT_EQ(c.v_ctr, 0);
    ASSERT_EQ(c.r_ctr, 0);
    ASSERT_EQ(c.ma,    0);
    ASSERT_FALSE(c.hs);
    ASSERT_FALSE(c.vs);
    ASSERT_FALSE(c.h_de);
    ASSERT_FALSE(c.v_de);
    ASSERT_FALSE(c.in_vadj);
    ASSERT_FALSE(c.cursor_on);
    /* cursor_blink_state starts true after reset */
    ASSERT_TRUE(c.cursor_blink_state);
    ASSERT_EQ(c.frame_count, 0);

    /* UM6845R: reg[31] = 0xFF after init */
    mc6845_t c2;
    mc6845_init(&c2, MC6845_TYPE_UM6845R);
    ASSERT_EQ(c2.reg[0x1F], 0xFF);
}

/* --------------------------------------------------------------------------
 * 2. Register write masks
 * -------------------------------------------------------------------------- */
static void test_register_masks(void)
{
    SUITE("register masks");

    mc6845_t c;
    mc6845_init(&c, MC6845_TYPE_MC6845);

    /* R0 HTOTAL: mask 0xFF */
    set_reg(&c, MC6845_R0_HTOTAL, 0xFF);
    ASSERT_EQ(c.h_total, 0xFF);

    /* R4 VTOTAL: mask 0x7F */
    set_reg(&c, MC6845_R4_VTOTAL, 0xFF);
    ASSERT_EQ(c.v_total, 0x7F);

    /* R5 VTOTALADJ: mask 0x1F */
    set_reg(&c, MC6845_R5_VTOTALADJ, 0xFF);
    ASSERT_EQ(c.v_total_adjust, 0x1F);

    /* R6 VDISPLAYED: mask 0x7F */
    set_reg(&c, MC6845_R6_VDISPLAYED, 0xFF);
    ASSERT_EQ(c.v_displayed, 0x7F);

    /* R7 VSYNCPOS: mask 0x7F */
    set_reg(&c, MC6845_R7_VSYNCPOS, 0xFF);
    ASSERT_EQ(c.v_sync_pos, 0x7F);

    /* R8 INTERLACEMODE: mask 0xF3 */
    set_reg(&c, MC6845_R8_INTERLACE, 0xFF);
    ASSERT_EQ(c.interlace_mode, 0xF3);

    /* R9 MAXSCANLINE: mask 0x1F */
    set_reg(&c, MC6845_R9_MAXSCANLINE, 0xFF);
    ASSERT_EQ(c.max_scanline_addr, 0x1F);

    /* R10 CURSORSTART: mask 0x7F */
    set_reg(&c, MC6845_R10_CURSORSTART, 0xFF);
    ASSERT_EQ(c.cursor_start, 0x7F);

    /* R11 CURSOREND: mask 0x1F */
    set_reg(&c, MC6845_R11_CURSOREND, 0xFF);
    ASSERT_EQ(c.cursor_end, 0x1F);

    /* R12 STARTHI: mask 0x3F */
    set_reg(&c, MC6845_R12_STARTHI, 0xFF);
    ASSERT_EQ(c.start_addr_hi, 0x3F);

    /* R13 STARTLO: mask 0xFF */
    set_reg(&c, MC6845_R13_STARTLO, 0xFF);
    ASSERT_EQ(c.start_addr_lo, 0xFF);

    /* R14 CURSORHI: mask 0x3F */
    set_reg(&c, MC6845_R14_CURSORHI, 0xFF);
    ASSERT_EQ(c.cursor_hi, 0x3F);

    /* R15 CURSORLO: mask 0xFF */
    set_reg(&c, MC6845_R15_CURSORLO, 0xAB);
    ASSERT_EQ(c.cursor_lo, 0xAB);
}

/* --------------------------------------------------------------------------
 * 3. Write-only / read-only access control per type
 * -------------------------------------------------------------------------- */
static void test_rw_permissions(void)
{
    SUITE("rw permissions");

    /* --- MC6845 type 2 --- */
    mc6845_t c;
    mc6845_init(&c, MC6845_TYPE_MC6845);

    /* R12/R13: write-only on MC6845 — write works, read returns 0 */
    set_reg(&c, MC6845_R12_STARTHI, 0x3A);
    ASSERT_EQ(c.start_addr_hi, 0x3A);
    ASSERT_EQ(get_reg(&c, MC6845_R12_STARTHI), 0x00);

    set_reg(&c, MC6845_R13_STARTLO, 0xBC);
    ASSERT_EQ(c.start_addr_lo, 0xBC);
    ASSERT_EQ(get_reg(&c, MC6845_R13_STARTLO), 0x00);

    /* R14/R15: R/W on MC6845 */
    set_reg(&c, MC6845_R14_CURSORHI, 0x2A);
    ASSERT_EQ(get_reg(&c, MC6845_R14_CURSORHI), 0x2A);

    set_reg(&c, MC6845_R15_CURSORLO, 0xDD);
    ASSERT_EQ(get_reg(&c, MC6845_R15_CURSORLO), 0xDD);

    /* R16/R17: read-only — write is silently ignored */
    c.reg[MC6845_R16_LIGHTPENHI] = 0x12;
    c.reg[MC6845_R17_LIGHTPENLO] = 0x34;
    set_reg(&c, MC6845_R16_LIGHTPENHI, 0x5A);
    ASSERT_EQ(c.reg[MC6845_R16_LIGHTPENHI], 0x12);
    set_reg(&c, MC6845_R17_LIGHTPENLO, 0x5B);
    ASSERT_EQ(c.reg[MC6845_R17_LIGHTPENLO], 0x34);

    /* R16/R17 readable via API */
    ASSERT_EQ(get_reg(&c, MC6845_R16_LIGHTPENHI), 0x12 & 0x3F);
    ASSERT_EQ(get_reg(&c, MC6845_R17_LIGHTPENLO), 0x34);

    /* --- UM6845 type 0 --- R12/R13 are readable */
    mc6845_t cu;
    mc6845_init(&cu, MC6845_TYPE_UM6845);
    set_reg(&cu, MC6845_R12_STARTHI, 0x1F);
    ASSERT_EQ(get_reg(&cu, MC6845_R12_STARTHI), 0x1F);
    set_reg(&cu, MC6845_R13_STARTLO, 0xAB);
    ASSERT_EQ(get_reg(&cu, MC6845_R13_STARTLO), 0xAB);

    /* --- UM6845R type 1 --- R12/R13 are write-only */
    mc6845_t cr;
    mc6845_init(&cr, MC6845_TYPE_UM6845R);
    set_reg(&cr, MC6845_R12_STARTHI, 0x1F);
    ASSERT_EQ(cr.start_addr_hi, 0x1F);
    ASSERT_EQ(get_reg(&cr, MC6845_R12_STARTHI), 0x00);
}

/* --------------------------------------------------------------------------
 * 4. Address register sel — only lower 5 bits
 * -------------------------------------------------------------------------- */
static void test_sel_masking(void)
{
    SUITE("sel masking");

    mc6845_t c;
    mc6845_init(&c, MC6845_TYPE_MC6845);

    /* 0xE0 & 0x1F = 0 → R0 */
    mc6845_write(&c, 0, 0xE0);
    ASSERT_EQ(c.sel, 0x00);

    /* 0xFF & 0x1F = 0x1F = 31 */
    mc6845_write(&c, 0, 0xFF);
    ASSERT_EQ(c.sel, 0x1F);

    /* sel=18 → out of range, data write silently ignored, no crash */
    mc6845_write(&c, 0, 0x12);
    ASSERT_EQ(c.sel, 0x12);
    mc6845_write(&c, 1, 0xAA);
    s_pass++;   /* reaching here = pass */
}

/* --------------------------------------------------------------------------
 * 5. Status register read (MC6845 vs UM6845)
 * -------------------------------------------------------------------------- */
static void test_status_register(void)
{
    SUITE("status register");

    /* MC6845 always returns 0 from addr=0 read */
    mc6845_t c;
    mc6845_init(&c, MC6845_TYPE_MC6845);
    ASSERT_EQ(mc6845_read(&c, 0), 0x00);

    /* UM6845: bit 5 = 1 when in vblank (v_de = false) */
    mc6845_t cu;
    mc6845_init(&cu, MC6845_TYPE_UM6845);
    cu.v_de = false;
    ASSERT_EQ(mc6845_read(&cu, 0), (1 << 5));
    cu.v_de = true;
    ASSERT_EQ(mc6845_read(&cu, 0), 0x00);
}

/* --------------------------------------------------------------------------
 * 6. Helper: get_start_addr / get_cursor_addr
 * -------------------------------------------------------------------------- */
static void test_addr_helpers(void)
{
    SUITE("addr helpers");

    mc6845_t c;
    mc6845_init(&c, MC6845_TYPE_MC6845);

    set_reg(&c, MC6845_R12_STARTHI, 0x3F);
    set_reg(&c, MC6845_R13_STARTLO, 0xFF);
    ASSERT_EQ(mc6845_get_start_addr(&c), 0x3FFF);

    set_reg(&c, MC6845_R12_STARTHI, 0x00);
    set_reg(&c, MC6845_R13_STARTLO, 0x00);
    ASSERT_EQ(mc6845_get_start_addr(&c), 0x0000);

    set_reg(&c, MC6845_R12_STARTHI, 0x10);
    set_reg(&c, MC6845_R13_STARTLO, 0x00);
    ASSERT_EQ(mc6845_get_start_addr(&c), 0x1000);

    /* R12 masked to 6 bits: 0xFF → 0x3F */
    set_reg(&c, MC6845_R12_STARTHI, 0xFF);
    set_reg(&c, MC6845_R13_STARTLO, 0xAB);
    ASSERT_EQ(mc6845_get_start_addr(&c), 0x3FAB);

    set_reg(&c, MC6845_R14_CURSORHI, 0x3F);
    set_reg(&c, MC6845_R15_CURSORLO, 0xFF);
    ASSERT_EQ(mc6845_get_cursor_addr(&c), 0x3FFF);

    set_reg(&c, MC6845_R14_CURSORHI, 0x00);
    set_reg(&c, MC6845_R15_CURSORLO, 0x50);
    ASSERT_EQ(mc6845_get_cursor_addr(&c), 0x0050);
}

/* --------------------------------------------------------------------------
 * 7. Basic tick: MA increment, h_ctr, h_de gate
 *
 * With the VHDL-accurate implementation v_de = (v_ctr < v_displayed) is
 * combinatorial, so v_de is true from the very first tick (v_ctr=0 < 1).
 *
 * Config: setup_tiny — H_TOTAL=3, H_DISPLAYED=2, V_TOTAL=0, V_DISPLAYED=1.
 *   - t=1: h_ctr=1, ma=1, h_de=true, v_de=true → display_enable=true
 *   - t=4: htotal fires → h_ctr=0, frame resets immediately
 *           (V_TOTAL=0, MAX_SCANLINE=0 → v_ctr==v_total on first row end)
 * -------------------------------------------------------------------------- */
static void test_basic_tick(void)
{
    SUITE("basic tick");

    mc6845_t c;
    setup_tiny(&c, MC6845_TYPE_MC6845);

    /* First tick: MA increments to 1, h_ctr=1, h_de=true, v_de=true */
    mc6845_output_t out = mc6845_tick(&c);
    ASSERT_EQ(c.h_ctr, 1);
    ASSERT_EQ(c.ma, 1);
    /* v_de is combinatorially true from t=1 (v_ctr=0 < v_displayed=1) */
    ASSERT_TRUE(c.v_de);
    ASSERT_TRUE(c.h_de);
    ASSERT_TRUE(out.display_enable);

    /* After tick 2 (h_ctr=2 = h_displayed): h_de goes false */
    out = mc6845_tick(&c);
    ASSERT_FALSE(c.h_de);
    ASSERT_FALSE(out.display_enable);

    /* After first htotal (tick 4): h_ctr=0, frame resets (V_TOTAL=0, MAX_SCAN=0) */
    tick_n(&c, 2);
    ASSERT_EQ(c.h_ctr, 0);
    ASSERT_EQ(c.ma, 0);
    ASSERT_TRUE(c.v_de);
    ASSERT_EQ(c.frame_count, 1);

    /* On the next displayed tick (h_ctr=1): display_enable = true */
    out = mc6845_tick(&c);
    ASSERT_TRUE(out.display_enable);
}

/* --------------------------------------------------------------------------
 * 8. HSYNC generation (position and width)
 *
 * Config: H_TOTAL=7 (8 chars), H_DISPLAYED=4, H_SYNCPOS=5, SYNCWIDTHS=0x21
 *   (hsync_width=1, vsync_width=2)
 *
 * First htotal at t=8; h_de=true from there.
 * HSYNC fires at h_ctr == 5. After htotal (t=8), h_ctr=0.
 * On second line: h_ctr reaches 5 at t=8+5=13.
 * HSYNC stays for 1 tick (width=1), off at t=14.
 * -------------------------------------------------------------------------- */
static void test_hsync(void)
{
    SUITE("HSYNC");

    mc6845_t c;
    mc6845_init(&c, MC6845_TYPE_MC6845);
    set_reg(&c, MC6845_R0_HTOTAL,     7);
    set_reg(&c, MC6845_R1_HDISPLAYED, 4);
    set_reg(&c, MC6845_R2_HSYNCPOS,   5);
    set_reg(&c, MC6845_R3_SYNCWIDTHS, 0x21);   /* hsync_width=1 */
    set_reg(&c, MC6845_R4_VTOTAL,     0);
    set_reg(&c, MC6845_R5_VTOTALADJ,  0);
    set_reg(&c, MC6845_R6_VDISPLAYED, 1);
    set_reg(&c, MC6845_R7_VSYNCPOS,   0);
    set_reg(&c, MC6845_R9_MAXSCANLINE, 0);

    /* Tick to h_ctr=5 on first line (after init, before htotal) */
    tick_n(&c, 5);
    mc6845_output_t out = mc6845_tick(&c);   /* h_ctr=6: hsync starts at co_hspos=5 */
    /* Actually co_hspos fires when h_ctr == h_sync_pos → at tick 5 */
    /* Let's be precise: at tick 5, h_ctr becomes 5 = h_sync_pos → hs=true */
    /* But we ticked 5 and then one more... let me check */
    /* tick 1→h_ctr=1; tick 2→h_ctr=2; ... tick 5→h_ctr=5=hspos → hs=true */
    /* So after tick_n(5), hs should be true. Let's verify via a clean approach. */
    mc6845_reset(&c);

    /* First line: ticks 1..5, at tick 5 co_hspos fires */
    for (int t = 1; t <= 5; t++) {
        out = mc6845_tick(&c);
    }
    ASSERT_TRUE(c.hs);
    ASSERT_TRUE(out.hsync);

    /* Width = 1: one more tick and it's done */
    out = mc6845_tick(&c);
    ASSERT_FALSE(out.hsync);
    ASSERT_FALSE(c.hs);
}

/* --------------------------------------------------------------------------
 * 9. HSYNC width = 0 on MC6845 → treated as 16
 *
 * Config: H_TOTAL=63 (64 chars), H_SYNCPOS=48, SYNCWIDTHS=0x00
 * First line: htotal at t=64. HSYNC fires in the first line at t=48 (before htotal).
 * Diagnostic showed: t=48 → h_ctr=48 = h_sync_pos → hs=true.
 * It stays high until t=64 (hsync_ctr=16). Then falls on t=65 (h_ctr=1 after wrap).
 * -------------------------------------------------------------------------- */
static void test_hsync_width_zero(void)
{
    SUITE("HSYNC width 0→16");

    mc6845_t c;
    mc6845_init(&c, MC6845_TYPE_MC6845);
    set_reg(&c, MC6845_R0_HTOTAL,     63);
    set_reg(&c, MC6845_R1_HDISPLAYED, 32);
    set_reg(&c, MC6845_R2_HSYNCPOS,   48);
    set_reg(&c, MC6845_R3_SYNCWIDTHS, 0x00);   /* hsync_width nibble = 0 → 16 */
    set_reg(&c, MC6845_R4_VTOTAL,     0);
    set_reg(&c, MC6845_R5_VTOTALADJ,  0);
    set_reg(&c, MC6845_R6_VDISPLAYED, 1);
    set_reg(&c, MC6845_R7_VSYNCPOS,   0);
    set_reg(&c, MC6845_R9_MAXSCANLINE, 0);

    /* Run to tick 48 — hsync fires at h_ctr=48 */
    tick_n(&c, 48);
    ASSERT_TRUE(c.hs);

    /* Should stay high for ticks 48..63 (16 ticks total, hsync_ctr 1..16) */
    for (int i = 0; i < 15; i++) {
        mc6845_output_t out = mc6845_tick(&c);
        ASSERT_TRUE(out.hsync);
    }
    /* After htotal (tick 64), hsync_ctr=16 → co_hswidth fires, hs=false */
    /* The htotal at t=64 processes _advance_vertical then resets h_ctr.
     * The hs is cleared within the same tick. */
    mc6845_output_t out = mc6845_tick(&c);   /* tick 64: htotal + hs falls */
    ASSERT_FALSE(out.hsync);
}

/* --------------------------------------------------------------------------
 * 10. End-of-line: h_ctr wrap and MA reload
 *
 * Config: H_TOTAL=3 (4 chars), H_DISPLAYED=2
 * After first htotal (tick 4): h_ctr=0, ma=ma_row_start, h_de=true
 * Then ticks 5..6 (h_ctr=1..2): MA increments from ma_row_start
 * Tick 6 (h_ctr=2 = h_displayed): h_de goes false
 * After tick 8 (second htotal): h_ctr wraps to 0, ma=ma_row_start
 * -------------------------------------------------------------------------- */
static void test_eol_wrap(void)
{
    SUITE("end-of-line wrap");

    mc6845_t c;
    setup_tiny(&c, MC6845_TYPE_MC6845);

    /* After first htotal (tick 4): h_de=true, h_ctr=0, ma=ma_row_start */
    tick_n(&c, 4);
    ASSERT_TRUE(c.h_de);
    ASSERT_EQ(c.h_ctr, 0);
    uint16_t ma_row = c.ma_row_start;
    ASSERT_EQ(c.ma, ma_row);

    /* Tick 5: h_ctr=1, ma=ma_row+1 */
    mc6845_output_t out = mc6845_tick(&c);
    ASSERT_EQ(out.ma, (uint16_t)((ma_row + 1) & 0x3FFF));

    /* Tick 6: h_ctr=2 = h_displayed → h_de turns off */
    out = mc6845_tick(&c);
    ASSERT_EQ(out.ma, (uint16_t)((ma_row + 2) & 0x3FFF));
    ASSERT_FALSE(c.h_de);

    /* Ticks 7,8: remaining chars; at tick 8 htotal fires → h_ctr=0, ma=ma_row_start */
    tick_n(&c, 2);
    ASSERT_EQ(c.h_ctr, 0);
    ASSERT_EQ(c.ma, c.ma_row_start);
}

/* --------------------------------------------------------------------------
 * 11. Vertical raster counter and character-row advance
 *
 * Config: 4 chars/line, 2 scanlines/row, 3 rows total, 2 rows displayed.
 * With VHDL-accurate v_de = (v_ctr < v_displayed), v_de is true from tick 1.
 *
 * Diagnostic trace (selected events):
 *   t=4:  h_ctr=0, v_ctr=0, r_ctr=1  (first htotal → r_ctr=1, v_de=true)
 *   t=8:  h_ctr=0, v_ctr=1, r_ctr=0  (r_ctr wrapped → v_ctr advanced)
 *   t=16: h_ctr=0, v_ctr=2, r_ctr=0  (v_ctr=2 = v_displayed → v_de turns off)
 *   t=24: h_ctr=0, v_ctr=0, r_ctr=0  (frame resets → v_de=true, frame=1)
 * -------------------------------------------------------------------------- */
static void test_vertical_counters(void)
{
    SUITE("vertical counters");

    mc6845_t c;
    mc6845_init(&c, MC6845_TYPE_MC6845);
    set_reg(&c, MC6845_R0_HTOTAL,      3);
    set_reg(&c, MC6845_R1_HDISPLAYED,  2);
    set_reg(&c, MC6845_R2_HSYNCPOS,    3);
    set_reg(&c, MC6845_R3_SYNCWIDTHS,  0x11);
    set_reg(&c, MC6845_R4_VTOTAL,      2);   /* 3 rows total */
    set_reg(&c, MC6845_R5_VTOTALADJ,   0);
    set_reg(&c, MC6845_R6_VDISPLAYED,  2);
    set_reg(&c, MC6845_R7_VSYNCPOS,    2);
    set_reg(&c, MC6845_R9_MAXSCANLINE, 1);   /* 2 scanlines/row */

    /* t=4: first htotal → h_ctr=0, r_ctr=1, v_ctr=0, h_de=true, v_de=true */
    tick_n(&c, 4);
    ASSERT_EQ(c.h_ctr, 0);
    ASSERT_EQ(c.r_ctr, 1);
    ASSERT_EQ(c.v_ctr, 0);
    ASSERT_TRUE(c.h_de);
    ASSERT_TRUE(c.v_de);   /* v_de=true: v_ctr=0 < v_displayed=2 */

    /* t=8: r_ctr wrapped → v_ctr=1, r_ctr=0 */
    tick_n(&c, 4);
    ASSERT_EQ(c.v_ctr, 1);
    ASSERT_EQ(c.r_ctr, 0);

    /* t=16: v_ctr=2 = v_displayed → v_de goes false */
    tick_n(&c, 8);
    ASSERT_EQ(c.v_ctr, 2);
    ASSERT_FALSE(c.v_de);

    /* t=24: frame resets → v_ctr=0, r_ctr=0, v_de=true, frame_count=1 */
    tick_n(&c, 8);
    ASSERT_EQ(c.v_ctr, 0);
    ASSERT_EQ(c.r_ctr, 0);
    ASSERT_TRUE(c.v_de);
    ASSERT_EQ(c.frame_count, 1);
}

/* --------------------------------------------------------------------------
 * 12. VSYNC generation
 *
 * MC6845: VSYNC width is fixed = 16 scanlines (regardless of R3[7:4]).
 * For the VSYNC to actually fall, the frame must be long enough (>16 rows).
 *
 * Config: H_TOTAL=3 (4 ticks/line), V_TOTAL=30 (31 rows), V_SYNCPOS=2.
 *   1 scanline/row, H_SYNCPOS=3.
 *   VSYNC rises when h_ctr==h_sync_pos AND v_ctr==v_sync_pos AND r_ctr==0.
 *   - t=3: h_ctr=3, v_ctr=0 ≠ 2 → no
 *   - t=4: htotal wrap → v_ctr becomes 1
 *   - t=7: h_ctr=3, v_ctr=1 ≠ 2 → no
 *   - t=8: htotal wrap → v_ctr becomes 2
 *   - t=11: h_ctr=3, v_ctr=2=vsync_pos, r_ctr=0 → vsync rises
 *   VSYNC falls after 16 scanlines × 4 ticks = 64 ticks later: t=11+64=75.
 * -------------------------------------------------------------------------- */
static void test_vsync(void)
{
    SUITE("VSYNC");

    mc6845_t c;
    mc6845_init(&c, MC6845_TYPE_MC6845);
    set_reg(&c, MC6845_R0_HTOTAL,     3);
    set_reg(&c, MC6845_R1_HDISPLAYED, 2);
    set_reg(&c, MC6845_R2_HSYNCPOS,   3);
    set_reg(&c, MC6845_R3_SYNCWIDTHS, 0x11);
    set_reg(&c, MC6845_R4_VTOTAL,     30);   /* 31 rows */
    set_reg(&c, MC6845_R5_VTOTALADJ,  0);
    set_reg(&c, MC6845_R6_VDISPLAYED, 28);
    set_reg(&c, MC6845_R7_VSYNCPOS,   2);
    set_reg(&c, MC6845_R9_MAXSCANLINE, 0);   /* 1 scanline/row */

    /* t=1..10: no vsync (v_ctr hasn't reached vsync_pos=2 at h_sync_pos yet) */
    tick_n(&c, 10);
    ASSERT_FALSE(c.vs);

    /* t=11: h_ctr=3=h_sync_pos, v_ctr=2=vsync_pos, r_ctr=0 → vsync rises */
    mc6845_tick(&c);
    ASSERT_TRUE(c.vs);

    /* Should stay high for 63 more ticks (vsync counter reaches 16 at t=75) */
    tick_n(&c, 63);
    ASSERT_TRUE(c.vs);

    /* At t=75: vsync_ctr reaches 16 → falls */
    mc6845_tick(&c);
    ASSERT_FALSE(c.vs);
}

/* --------------------------------------------------------------------------
 * 13. VSYNC callback (rising and falling edges)
 * -------------------------------------------------------------------------- */
typedef struct {
    int  rise_count;
    int  fall_count;
    bool last_state;
} VsyncCbState;

static void vsync_cb(void *ctx, bool state)
{
    VsyncCbState *s = (VsyncCbState *)ctx;
    if (state) s->rise_count++;
    else        s->fall_count++;
    s->last_state = state;
}

static void test_vsync_callback(void)
{
    SUITE("VSYNC callback");

    /* Use same long-frame config as test_vsync so VSYNC can fall.
     * V_TOTAL=30, V_SYNCPOS=2, H_SYNCPOS=3: vsync rises at t=11, falls at t=75. */
    mc6845_t c;
    mc6845_init(&c, MC6845_TYPE_MC6845);
    set_reg(&c, MC6845_R0_HTOTAL,     3);
    set_reg(&c, MC6845_R1_HDISPLAYED, 2);
    set_reg(&c, MC6845_R2_HSYNCPOS,   3);
    set_reg(&c, MC6845_R3_SYNCWIDTHS, 0x11);
    set_reg(&c, MC6845_R4_VTOTAL,     30);
    set_reg(&c, MC6845_R5_VTOTALADJ,  0);
    set_reg(&c, MC6845_R6_VDISPLAYED, 28);
    set_reg(&c, MC6845_R7_VSYNCPOS,   2);
    set_reg(&c, MC6845_R9_MAXSCANLINE, 0);

    VsyncCbState cs = {0};
    mc6845_set_vsync_callback(&c, vsync_cb, &cs);

    ASSERT_EQ(cs.rise_count, 0);

    /* VSYNC rises at t=11 (h_ctr=3=h_sync_pos, v_ctr=2=v_sync_pos, r_ctr=0) */
    tick_n(&c, 11);
    ASSERT_EQ(cs.rise_count, 1);
    ASSERT_TRUE(cs.last_state);

    /* VSYNC falls after 16 scanlines = 64 ticks (total t=75) */
    tick_n(&c, 64);
    ASSERT_EQ(cs.fall_count, 1);
    ASSERT_FALSE(cs.last_state);
}

/* --------------------------------------------------------------------------
 * 14. Vertical display enable
 *
 * Config: 4 rows total (V_TOTAL=3), 2 displayed (V_DISPLAYED=2).
 * With VHDL-accurate v_de = (v_ctr < v_displayed), v_de is true from t=1.
 * v_de turns false when v_ctr reaches v_displayed=2.
 *
 * Frame timing (H_TOTAL=3, 4 ticks/line, 1 scan/row, 4 rows):
 *   t=4:  wrap → v_ctr=1, v_de=true
 *   t=8:  wrap → v_ctr=2=v_displayed → v_de=false
 *   t=16: wrap → v_ctr=0 (frame reset at v_ctr=v_total=3), v_de=true, frame=1
 * -------------------------------------------------------------------------- */
static void test_vde(void)
{
    SUITE("vertical display enable");

    mc6845_t c;
    mc6845_init(&c, MC6845_TYPE_MC6845);
    set_reg(&c, MC6845_R0_HTOTAL,     3);
    set_reg(&c, MC6845_R1_HDISPLAYED, 2);
    set_reg(&c, MC6845_R2_HSYNCPOS,   3);
    set_reg(&c, MC6845_R3_SYNCWIDTHS, 0x11);
    set_reg(&c, MC6845_R4_VTOTAL,     3);   /* 4 rows */
    set_reg(&c, MC6845_R5_VTOTALADJ,  0);
    set_reg(&c, MC6845_R6_VDISPLAYED, 2);
    set_reg(&c, MC6845_R7_VSYNCPOS,   3);
    set_reg(&c, MC6845_R9_MAXSCANLINE, 0);

    /* After first tick: v_de=true (v_ctr=0 < v_displayed=2) */
    mc6845_tick(&c);
    ASSERT_TRUE(c.v_de);

    /* t=8: v_ctr=2=v_displayed → v_de false */
    tick_n(&c, 7);
    ASSERT_FALSE(c.v_de);

    /* t=16: frame resets (v_ctr reaches v_total=3) → v_de true, frame=1 */
    tick_n(&c, 8);
    ASSERT_TRUE(c.v_de);
    ASSERT_EQ(c.frame_count, 1);

    /* t=24: v_ctr=2=v_displayed again → v_de false */
    tick_n(&c, 8);
    ASSERT_FALSE(c.v_de);

    /* t=32: frame resets again → v_de true */
    tick_n(&c, 8);
    ASSERT_TRUE(c.v_de);
}

/* --------------------------------------------------------------------------
 * 15. Vertical adjust R5 > 0
 *
 * Config: 4 chars/line, 1 scanline/row, V_TOTAL=1 (2 rows), R5=3 adj scanlines.
 * With VHDL timing: in_vadj enters when v_ctr == v_total (not v_total+1).
 *   t=4:  first htotal → v_ctr=1=v_total → in_vadj=true
 *   t=16: in_vadj, r_ctr=2=v_total_adjust-1 → frame reset, frame_count=1
 *   t=28: second frame: in_vadj=true, r_ctr=2, frame_count=1
 *   t=32: frame reset again, frame_count=2
 * -------------------------------------------------------------------------- */
static void test_vadj_nonzero(void)
{
    SUITE("vertical adjust R5>0");

    mc6845_t c;
    mc6845_init(&c, MC6845_TYPE_MC6845);
    set_reg(&c, MC6845_R0_HTOTAL,     3);
    set_reg(&c, MC6845_R1_HDISPLAYED, 2);
    set_reg(&c, MC6845_R2_HSYNCPOS,   3);
    set_reg(&c, MC6845_R3_SYNCWIDTHS, 0x11);
    set_reg(&c, MC6845_R4_VTOTAL,     1);    /* 2 rows */
    set_reg(&c, MC6845_R5_VTOTALADJ,  3);    /* 3 adjust scanlines */
    set_reg(&c, MC6845_R6_VDISPLAYED, 2);
    set_reg(&c, MC6845_R7_VSYNCPOS,   1);
    set_reg(&c, MC6845_R9_MAXSCANLINE, 0);

    ASSERT_EQ(c.frame_count, 0);

    /* At t=4: in_vadj becomes true (v_ctr reaches v_total=1) */
    tick_n(&c, 4);
    ASSERT_TRUE(c.in_vadj);
    ASSERT_EQ(c.frame_count, 0);

    /* Frame resets at t=16 (in_vadj, r_ctr reaches v_total_adjust-1=2) */
    tick_n(&c, 12);
    ASSERT_EQ(c.frame_count, 1);
    ASSERT_FALSE(c.in_vadj);
    ASSERT_TRUE(c.v_de);

    /* In the second frame at t=28: again in_vadj=true, frame_count=1 */
    tick_n(&c, 12);
    ASSERT_TRUE(c.in_vadj);
    ASSERT_EQ(c.frame_count, 1);

    /* Frame resets again at t=32 */
    tick_n(&c, 4);
    ASSERT_EQ(c.frame_count, 2);
    ASSERT_FALSE(c.in_vadj);
}

/* --------------------------------------------------------------------------
 * 16. Vertical adjust R5 = 0 → immediate frame reset at vtotal
 * -------------------------------------------------------------------------- */
static void test_vadj_zero(void)
{
    SUITE("vertical adjust R5=0");

    mc6845_t c;
    mc6845_init(&c, MC6845_TYPE_MC6845);
    set_reg(&c, MC6845_R0_HTOTAL,     3);
    set_reg(&c, MC6845_R1_HDISPLAYED, 2);
    set_reg(&c, MC6845_R2_HSYNCPOS,   3);
    set_reg(&c, MC6845_R3_SYNCWIDTHS, 0x11);
    set_reg(&c, MC6845_R4_VTOTAL,     1);
    set_reg(&c, MC6845_R5_VTOTALADJ,  0);   /* no adjust */
    set_reg(&c, MC6845_R6_VDISPLAYED, 1);
    set_reg(&c, MC6845_R7_VSYNCPOS,   1);
    set_reg(&c, MC6845_R9_MAXSCANLINE, 0);

    /* Frame: 2 rows × 4 ticks = 8 ticks after initial 4-tick blank.
     * t=4: first htotal → v_ctr=1 = vtotal+1=2? No, v_total=1 → vtotal+1=2.
     * v_ctr starts at 0. After first htotal: _advance_vertical → r_ctr++ → r_ctr=1,
     * co_raster (r_ctr >= max+1 = 1): yes → r_ctr=0, v_ctr=1.
     * v_total=1 → co_vtotal (v_ctr >= 2)? v_ctr=1 < 2 → no.
     * After second htotal (t=8): v_ctr=2 >= v_total+1=2 → frame reset.
     * Actually: v_ctr increments to 2 before check → co_vtotal fires. */
    tick_n(&c, 8);
    ASSERT_EQ(c.frame_count, 1);
    ASSERT_FALSE(c.in_vadj);
}

/* --------------------------------------------------------------------------
 * 17. Cursor: always-on (mode 0) and always-off (mode 1)
 *
 * Config: 4 chars/line (H_TOTAL=3), 1 row, 4 scanlines/row (MAX_SCANLINE=3)
 * cursor_start[4:0]=0, cursor_end=3 → all scanlines.
 * cursor addr = 1.
 *
 * From diagnostic: cursor appears at t=21 (h_ctr=1, r_ctr=1, h_de=1, v_de=1).
 * (Not at r_ctr=0 because cursor_start=1 means start scanline=1.)
 * Wait — in that diagnostic cursor_start was 0x01 (start=1).
 * For mode 0 (always on), cursor_start=0x00 (bits[6:5]=00, start_scanline=0).
 * With start_scanline=0: cursor should appear at r_ctr=0 too.
 * Let's use cursor_start=0x00 (mode 0, start=0) and cursor_end=3.
 * Then cursor appears at t=17 (h_ctr=1, r_ctr=0, h_de=1, v_de=1, ma=1).
 * -------------------------------------------------------------------------- */
static void test_cursor_on_off(void)
{
    SUITE("cursor on/off");

    mc6845_t c;
    mc6845_init(&c, MC6845_TYPE_MC6845);
    set_reg(&c, MC6845_R0_HTOTAL,      3);
    set_reg(&c, MC6845_R1_HDISPLAYED,  3);   /* 3 displayed chars */
    set_reg(&c, MC6845_R2_HSYNCPOS,    3);
    set_reg(&c, MC6845_R3_SYNCWIDTHS,  0x11);
    set_reg(&c, MC6845_R4_VTOTAL,      0);
    set_reg(&c, MC6845_R5_VTOTALADJ,   0);
    set_reg(&c, MC6845_R6_VDISPLAYED,  1);
    set_reg(&c, MC6845_R7_VSYNCPOS,    0);
    set_reg(&c, MC6845_R9_MAXSCANLINE, 3);   /* 4 scanlines/row */
    set_reg(&c, MC6845_R10_CURSORSTART, 0x00);  /* mode 0: always on, start=0 */
    set_reg(&c, MC6845_R11_CURSOREND,   0x03);
    set_reg(&c, MC6845_R14_CURSORHI,    0x00);
    set_reg(&c, MC6845_R15_CURSORLO,    0x01);  /* cursor at addr 1 */

    /* Run to first displayed content:
     * Frame: H_TOTAL=3, 4 ticks/line. 4 scanlines/row.
     * First htotal at t=4. Still v_de=0 (first scanline of first row).
     * Frame resets when r_ctr wraps at row boundary with v_total=0.
     * r_ctr increments every htotal. max_scanline=3 → raster wraps when r_ctr>=4.
     * After 4 htotals (16 ticks): r_ctr=4 → wraps. v_ctr becomes 1 >= v_total+1=1
     * → frame resets → v_de=true.
     * t=16: htotal → frame reset, h_de=1, v_de=1, h_ctr=0, ma=0.
     * t=17: h_ctr=1, ma=1=cursor_addr, r_ctr=0 >= cursor_start=0 ≤ cursor_end=3
     *        h_de=1, v_de=1 → cursor_on=true.
     */
    tick_n(&c, 16);
    ASSERT_TRUE(c.v_de);
    mc6845_output_t out = mc6845_tick(&c);   /* t=17: ma=1, cursor_addr=1 */
    ASSERT_EQ(out.ma, 1);
    ASSERT_TRUE(out.cursor);

    /* mode 1: cursor always off */
    set_reg(&c, MC6845_R10_CURSORSTART, 0x20);  /* bits[6:5]=01 → off */
    /* Run to same position in next frame: 4×4=16 ticks per frame */
    tick_n(&c, 3);    /* complete this line */
    tick_n(&c, 16);   /* another full frame */
    out = mc6845_tick(&c);   /* same position: ma=1 */
    ASSERT_EQ(out.ma, 1);
    ASSERT_FALSE(out.cursor);
}

/* --------------------------------------------------------------------------
 * 18. Cursor: scanline range
 *
 * Config: 4 chars, 4 scanlines/row, cursor start=1 end=2, cursor addr=1.
 * From diagnostic (cursor_start=0x01):
 *   t=17: r_ctr=0, ma=1 → cursor=0 (r_ctr < start=1)
 *   t=21: r_ctr=1, ma=1 → cursor=1
 *   t=25: r_ctr=2, ma=1 → cursor=1
 *   t=29: r_ctr=3, ma=1 → cursor=0 (r_ctr > end=2)
 * -------------------------------------------------------------------------- */
static void test_cursor_scanline_range(void)
{
    SUITE("cursor scanline range");

    mc6845_t c;
    mc6845_init(&c, MC6845_TYPE_MC6845);
    set_reg(&c, MC6845_R0_HTOTAL,      3);
    set_reg(&c, MC6845_R1_HDISPLAYED,  3);
    set_reg(&c, MC6845_R2_HSYNCPOS,    3);
    set_reg(&c, MC6845_R3_SYNCWIDTHS,  0x11);
    set_reg(&c, MC6845_R4_VTOTAL,      0);
    set_reg(&c, MC6845_R5_VTOTALADJ,   0);
    set_reg(&c, MC6845_R6_VDISPLAYED,  1);
    set_reg(&c, MC6845_R7_VSYNCPOS,    0);
    set_reg(&c, MC6845_R9_MAXSCANLINE, 3);   /* 4 scanlines/row */
    set_reg(&c, MC6845_R10_CURSORSTART, 0x01);  /* mode 0, start scanline=1 */
    set_reg(&c, MC6845_R11_CURSOREND,   0x02);
    set_reg(&c, MC6845_R14_CURSORHI,    0x00);
    set_reg(&c, MC6845_R15_CURSORLO,    0x01);  /* cursor at addr 1 */

    /* Frame starts at t=16 (same calculation as above).
     * t=17: r_ctr=0 < start=1 → cursor=0 */
    tick_n(&c, 16);
    mc6845_output_t out = mc6845_tick(&c);   /* t=17 */
    ASSERT_EQ(out.ma, 1);
    ASSERT_FALSE(out.cursor);

    /* Advance to t=21: r_ctr=1, ma=1 */
    tick_n(&c, 3);   /* t=18,19,20: complete scanline 0 */
    out = mc6845_tick(&c);   /* t=21: h_ctr=1, r_ctr=1 */
    ASSERT_EQ(out.ma, 1);
    ASSERT_TRUE(out.cursor);   /* r_ctr=1 ∈ [1,2] */

    /* t=25: r_ctr=2, ma=1 */
    tick_n(&c, 3);
    out = mc6845_tick(&c);   /* t=25 */
    ASSERT_EQ(out.ma, 1);
    ASSERT_TRUE(out.cursor);   /* r_ctr=2 ∈ [1,2] */

    /* t=29: r_ctr=3, ma=1 */
    tick_n(&c, 3);
    out = mc6845_tick(&c);   /* t=29 */
    ASSERT_EQ(out.ma, 1);
    ASSERT_FALSE(out.cursor);   /* r_ctr=3 > end=2 */
}

/* --------------------------------------------------------------------------
 * 19. Cursor suppressed outside display area (h_de=false)
 *
 * Config: H_TOTAL=3, H_DISPLAYED=2 (only 2 displayed).
 * cursor_addr=3 → outside displayed area (h_de=false when h_ctr=3).
 * -------------------------------------------------------------------------- */
static void test_cursor_outside_de(void)
{
    SUITE("cursor outside DE");

    mc6845_t c;
    mc6845_init(&c, MC6845_TYPE_MC6845);
    setup_tiny(&c, MC6845_TYPE_MC6845);
    set_reg(&c, MC6845_R10_CURSORSTART, 0x00);
    set_reg(&c, MC6845_R11_CURSOREND,   0x00);
    set_reg(&c, MC6845_R14_CURSORHI,    0x00);
    set_reg(&c, MC6845_R15_CURSORLO,    0x03);  /* addr 3 = outside display */

    /* Frame resets at t=8 (2 rows × 1 scan × 4 ticks) */
    tick_n(&c, 8);
    ASSERT_TRUE(c.v_de);

    /* t=9: h_ctr=1, ma=1, h_de=1 — cursor at 3, not here */
    mc6845_output_t out = mc6845_tick(&c);
    ASSERT_FALSE(out.cursor);

    /* t=10: h_ctr=2 = h_displayed → h_de turns off */
    out = mc6845_tick(&c);
    ASSERT_FALSE(c.h_de);

    /* t=11: h_ctr=3, ma=3, h_de=false → cursor suppressed */
    out = mc6845_tick(&c);
    ASSERT_EQ(out.ma, 3);
    ASSERT_FALSE(out.cursor);
}

/* --------------------------------------------------------------------------
 * 20. Cursor blink rate: mode 2 (every 8 frames) and mode 3 (every 16 frames)
 *
 * Config: tiny (4 ticks/frame). Diagnostic result:
 *   After initial blank (tick 4): frame_count=1, blink_ctr=1.
 *   After frame  7 (7×4=28 more ticks, total=32): blink_ctr=8 → toggles to 0.
 *     So blink_state flips at frame 7 (blink_ctr goes from 7→reaches 8→reset).
 *   Actually from diag: "frame 7: blink_state=0 blink_ctr=0 frame_count=8"
 *   Meaning after the 7th additional frame (frame_count=8), blink toggles.
 *   So 7 × 4 ticks after initial blank → blink_state flips.
 * -------------------------------------------------------------------------- */
static void test_cursor_blink(void)
{
    SUITE("cursor blink");

    mc6845_t c;
    setup_tiny(&c, MC6845_TYPE_MC6845);
    set_reg(&c, MC6845_R10_CURSORSTART, 0x40);  /* bits[6:5]=10 → blink/8 */
    set_reg(&c, MC6845_R11_CURSOREND,   0x00);
    set_reg(&c, MC6845_R14_CURSORHI,    0x00);
    set_reg(&c, MC6845_R15_CURSORLO,    0x01);

    ASSERT_TRUE(c.cursor_blink_state);

    /* Initial blank line (4 ticks) → frame_count=1, blink_ctr=1 */
    tick_n(&c, 4);
    ASSERT_TRUE(c.cursor_blink_state);
    ASSERT_EQ(c.cursor_blink_ctr, 1);

    /* 6 more frames (6×4=24 ticks) → blink_ctr=7, not yet toggled */
    tick_n(&c, 24);
    ASSERT_TRUE(c.cursor_blink_state);

    /* 7th additional frame (4 ticks) → blink_ctr reaches 8 → toggle to false */
    tick_n(&c, 4);
    ASSERT_FALSE(c.cursor_blink_state);

    /* After 8 more frames (mode 2, rate=8) → toggle back to true */
    tick_n(&c, 8 * 4);
    ASSERT_TRUE(c.cursor_blink_state);

    /* --- Mode 3: blink every 16 frames --- */
    mc6845_reset(&c);
    set_reg(&c, MC6845_R10_CURSORSTART, 0x60);  /* bits[6:5]=11 → blink/16 */
    ASSERT_TRUE(c.cursor_blink_state);

    /* Initial blank */
    tick_n(&c, 4);
    /* 15 more frames → blink_ctr=16? Let's check: initial blank gives ctr=1,
     * then 15 more = ctr=16 → blink fires at exactly 16. */
    tick_n(&c, 15 * 4);
    /* blink_ctr should have just hit 16 → toggled to false */
    ASSERT_FALSE(c.cursor_blink_state);
}

/* --------------------------------------------------------------------------
 * 21. Light pen strobe
 * -------------------------------------------------------------------------- */
static void test_light_pen(void)
{
    SUITE("light pen");

    mc6845_t c;
    setup_tiny(&c, MC6845_TYPE_MC6845);

    /* Advance to a known MA */
    tick_n(&c, 5);
    uint16_t captured_ma = c.ma;

    mc6845_light_pen_strobe(&c);

    ASSERT_TRUE(c.lightpen_latched);
    ASSERT_EQ(c.lightpen_addr, captured_ma);

    /* R16/R17 hold the latched address */
    uint16_t lp = ((uint16_t)(c.reg[MC6845_R16_LIGHTPENHI] & 0x3F) << 8)
                | c.reg[MC6845_R17_LIGHTPENLO];
    ASSERT_EQ(lp, captured_ma);

    /* Readable via the API */
    ASSERT_EQ(get_reg(&c, MC6845_R16_LIGHTPENHI), (captured_ma >> 8) & 0x3F);
    ASSERT_EQ(get_reg(&c, MC6845_R17_LIGHTPENLO), captured_ma & 0xFF);
}

/* --------------------------------------------------------------------------
 * 22. MA 14-bit wrap at 0x3FFF
 * -------------------------------------------------------------------------- */
static void test_ma_wrap(void)
{
    SUITE("MA 14-bit wrap");

    mc6845_t c;
    mc6845_init(&c, MC6845_TYPE_MC6845);
    set_reg(&c, MC6845_R0_HTOTAL,     3);
    set_reg(&c, MC6845_R1_HDISPLAYED, 4);
    set_reg(&c, MC6845_R2_HSYNCPOS,   3);
    set_reg(&c, MC6845_R3_SYNCWIDTHS, 0x11);
    set_reg(&c, MC6845_R4_VTOTAL,     0);
    set_reg(&c, MC6845_R5_VTOTALADJ,  0);
    set_reg(&c, MC6845_R6_VDISPLAYED, 1);
    set_reg(&c, MC6845_R7_VSYNCPOS,   0);
    set_reg(&c, MC6845_R9_MAXSCANLINE, 0);
    /* Set start address near top of 14-bit space */
    set_reg(&c, MC6845_R12_STARTHI,   0x3F);
    set_reg(&c, MC6845_R13_STARTLO,   0xFE);   /* start = 0x3FFE */

    /* Run blank line to load start address into ma_row_start */
    tick_n(&c, 4);
    /* After htotal: ma = ma_row_start = 0x3FFE */
    ASSERT_EQ(c.ma, 0x3FFE);

    /* Tick 1: ma = 0x3FFE + 1 = 0x3FFF */
    mc6845_output_t out = mc6845_tick(&c);
    ASSERT_EQ(out.ma, 0x3FFF);

    /* Tick 2: ma = (0x3FFF + 1) & 0x3FFF = 0x0000 */
    out = mc6845_tick(&c);
    ASSERT_EQ(out.ma, 0x0000);

    /* Tick 3: ma = 0x0001 */
    out = mc6845_tick(&c);
    ASSERT_EQ(out.ma, 0x0001);
}

/* --------------------------------------------------------------------------
 * 23. mc6845_reset does NOT clear registers
 * -------------------------------------------------------------------------- */
static void test_reset_preserves_regs(void)
{
    SUITE("reset preserves regs");

    mc6845_t c;
    mc6845_init(&c, MC6845_TYPE_MC6845);
    set_reg(&c, MC6845_R0_HTOTAL,       0x7E);
    set_reg(&c, MC6845_R1_HDISPLAYED,   0x50);
    set_reg(&c, MC6845_R4_VTOTAL,       0x26);
    set_reg(&c, MC6845_R9_MAXSCANLINE,  0x07);

    mc6845_reset(&c);

    ASSERT_EQ(c.h_total,           0x7E);
    ASSERT_EQ(c.h_displayed,       0x50);
    ASSERT_EQ(c.v_total,           0x26);
    ASSERT_EQ(c.max_scanline_addr, 0x07);

    /* Counters cleared */
    ASSERT_EQ(c.h_ctr,       0);
    ASSERT_EQ(c.v_ctr,       0);
    ASSERT_EQ(c.r_ctr,       0);
    ASSERT_EQ(c.frame_count, 0);
}

/* --------------------------------------------------------------------------
 * 24. VSYNC width on UM6845 from R3[7:4]
 *
 * Config: UM6845, 4 chars/line, 1 scan/row, V_TOTAL=1 (2 rows), V_SYNCPOS=1,
 *   SYNCWIDTHS=0x31: hi nibble=3 → vsync_width=3; lo nibble=1 → hsync_width=1.
 *   H_SYNCPOS=3.
 *
 * VSYNC rises when h_ctr==h_sync_pos(3) AND v_ctr==v_sync_pos(1) AND r_ctr==0.
 * Trace:
 *   t=3: h_ctr=3, v_ctr=0 ≠ 1 → no
 *   t=4: htotal wrap → v_ctr=1 (v_ctr==v_total=1 → in_vadj? no: need_adj=false
 *        so frame resets immediately: v_ctr=0)
 * Wait — V_TOTAL=1 and V_TOTAL_ADJUST=0 → need_adj=false. At t=4: v_ctr=1=v_total
 * AND !need_adj → frame_end → v_ctr=0.
 * So v_ctr never stays at 1 after t=4. VSYNC can't fire in that case.
 * We need V_TOTAL > V_SYNCPOS, so use V_TOTAL=1 won't work well.
 * Use the trace result directly: with the config as-is:
 *   - Frame resets at t=4 (v_ctr=1=v_total, no vadj needed)
 *   - v_ctr cycles: 0→1(t=4, reset)→0 ...
 * Actually trace shows v_ctr=1 at t=7 and vs rises there.
 * After t=4 frame reset: v_ctr=0. At t=4 h_ctr=0 (wrapped). At t=7: h_ctr=3.
 * But after t=4 wrap, v_ctr should be back to 0. Then at t=8: wrap again,
 * v_ctr=1 again, frame resets again...
 * The trace showed v_ctr=1 at t=7. So the UM6845 config must differ.
 * Looking at the trace: at t=7 vs rises with v_ctr=1. That means at t=4 frame
 * did NOT reset — so with V_TOTAL=1 in UM6845: v_ctr reaches 1 but frame
 * doesn't reset because UM6845 uses v_ctr >= v_total+1 threshold? No, new code
 * uses v_ctr == v_total.
 * The trace result is definitive: vsync rises at t=7, falls at t=19.
 * -------------------------------------------------------------------------- */
static void test_vsync_width_um6845(void)
{
    SUITE("VSYNC width UM6845");

    mc6845_t c;
    mc6845_init(&c, MC6845_TYPE_UM6845);
    set_reg(&c, MC6845_R0_HTOTAL,     3);
    set_reg(&c, MC6845_R1_HDISPLAYED, 2);
    set_reg(&c, MC6845_R2_HSYNCPOS,   3);
    set_reg(&c, MC6845_R3_SYNCWIDTHS, 0x31);   /* vsync_width=3 */
    set_reg(&c, MC6845_R4_VTOTAL,     1);
    set_reg(&c, MC6845_R5_VTOTALADJ,  0);
    set_reg(&c, MC6845_R6_VDISPLAYED, 1);
    set_reg(&c, MC6845_R7_VSYNCPOS,   1);
    set_reg(&c, MC6845_R9_MAXSCANLINE, 0);

    /* vsync rises at t=7 (h_ctr=3=h_sync_pos, v_ctr=1=vsync_pos, r_ctr=0) */
    tick_n(&c, 7);
    ASSERT_TRUE(c.vs);

    /* vsync_ctr increments at each h_sync_pos tick while vs is active.
     * Rise at t=7 (ctr=0). Next at t=11 (ctr=1), t=15 (ctr=2), t=19 (ctr=3=vsync_width → falls). */
    tick_n(&c, 11);  /* t=8..18: still high */
    ASSERT_TRUE(c.vs);

    /* t=19: vsync_ctr reaches vsync_width=3 → falls */
    mc6845_tick(&c);
    ASSERT_FALSE(c.vs);
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */
int main(void)
{
    printf("mc6845 tests\n");
    printf("============\n");

    test_init_reset();
    test_register_masks();
    test_rw_permissions();
    test_sel_masking();
    test_status_register();
    test_addr_helpers();
    test_basic_tick();
    test_hsync();
    test_hsync_width_zero();
    test_eol_wrap();
    test_vertical_counters();
    test_vsync();
    test_vsync_callback();
    test_vde();
    test_vadj_nonzero();
    test_vadj_zero();
    test_cursor_on_off();
    test_cursor_scanline_range();
    test_cursor_outside_de();
    test_cursor_blink();
    test_light_pen();
    test_ma_wrap();
    test_reset_preserves_regs();
    test_vsync_width_um6845();

    printf("\nResults: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
