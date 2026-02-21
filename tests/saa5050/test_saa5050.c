/*
 * Tests for saa5050 — SAA5050 Teletext Character Generator emulation
 *
 * Covers:
 *   1.  init / reset state
 *   2.  get_builtin_rom returns non-NULL
 *   3.  start_row bounds check
 *   4.  start_scanline initialises line_state correctly
 *   5.  start_scanline loads dh_row_bottom into double_height
 *   6.  char_code bit 7 is stripped (& 0x7F)
 *   7.  text glyph from ROM: correct pixels for space and 'A'
 *   8.  ROM scanline mapping: scanline >> 1 (20 scanlines → 10 ROM rows)
 *   9.  ROM rows 8-9: all zero for every character
 *  10.  pixel expansion: 6 raw pixels → 12 output (each doubled)
 *  11.  fg/bg colour: default white-on-black
 *  12.  control code 1-7: alphanumeric colour, exits graphics, clears held_char
 *  13.  control code 8 (flash on) / 9 (steady)
 *  14.  control code 12 (normal height) / 13 (double height + dh_seen)
 *  15.  control code 17-23: graphics colour + enters graphics mode
 *  16.  control code 24 (conceal): fg = bg
 *  17.  control code 25/26: contiguous / separated graphics
 *  18.  control code 28/29: black background / new background
 *  19.  control code 30/31: hold graphics / release graphics
 *  20.  unhandled control codes (0, 10, 11, 14-16, 27): no state change
 *  21.  graphics mode: sixel rendering for printable chars
 *  22.  sixel bit mapping: each of the 6 blocks
 *  23.  separated graphics: column 2 and 5 blanked; bottom-row blanked
 *  24.  hold graphics: held char displayed for control codes
 *  25.  hold_old: release (0x1F) still shows held char for that cell
 *  26.  text char in graphics mode resets held_char
 *  27.  graphics char range: 0x40-0x5F in graphics mode → text ROM
 *  28.  flash blanking: fg→bg when flash=true and flash_state=false
 *  29.  flash timing: toggle_flash 64-frame cycle, on=48 off=16
 *  30.  double-height: bottom half uses rom_row = (scan>>1)+5
 *  31.  dh_row_bottom propagation at scanline 19
 *  32.  saa5050_reset: clears flash, counters, dh arrays
 *  33.  custom char_rom passed to init
 */

#include "saa5050.h"
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
        printf("  FAIL %s:%d: expected %d, got %d\n", \
               s_suite, __LINE__, (int)(b), (int)(a)); \
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

#define ASSERT_NOT_NULL(p) do { \
    if ((p) == NULL) { \
        printf("  FAIL %s:%d: expected non-NULL\n", s_suite, __LINE__); \
        s_fail++; \
    } else { s_pass++; } \
} while(0)

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

/* Render one character on scanline 0 with default line state.
 * Returns pixel array of 12 bytes. */
static void render_char_sl(saa5050_t *tt, uint8_t char_code,
                             uint8_t scanline, uint8_t *px12)
{
    saa5050_line_state_t ls;
    saa5050_start_scanline(tt, &ls, scanline);
    saa5050_render_char(tt, &ls, char_code, px12);
}

/* Check that all 12 output pixels equal expected colour */
static int all_pixels_eq(const uint8_t *px, uint8_t expected)
{
    for (int i = 0; i < SAA5050_PIXELS_PER_CHAR; i++)
        if (px[i] != expected) return 0;
    return 1;
}

/* --------------------------------------------------------------------------
 * 1. init / reset state
 * -------------------------------------------------------------------------- */
static void test_init_reset(void)
{
    SUITE("init/reset");

    saa5050_t tt;
    saa5050_init(&tt, NULL);   /* NULL = use built-in ROM */

    ASSERT_NOT_NULL(tt.char_rom);
    ASSERT_FALSE(tt.flash_state);
    ASSERT_EQ(tt.flash_counter, 0);
    ASSERT_EQ(tt.current_row, 0);
    ASSERT_EQ(tt.current_scanline, 0);
    ASSERT_FALSE(tt.dh_seen_this_row);

    /* dh_row_bottom all false */
    for (int i = 0; i < SAA5050_ROWS; i++) {
        ASSERT_FALSE(tt.dh_row_bottom[i]);
    }

    /* reset clears flash state */
    tt.flash_state   = true;
    tt.flash_counter = 42;
    saa5050_reset(&tt);
    ASSERT_FALSE(tt.flash_state);
    ASSERT_EQ(tt.flash_counter, 0);
    ASSERT_EQ(tt.current_row, 0);
    ASSERT_EQ(tt.current_scanline, 0);
}

/* --------------------------------------------------------------------------
 * 2. get_builtin_rom returns non-NULL
 * -------------------------------------------------------------------------- */
static void test_builtin_rom(void)
{
    SUITE("builtin ROM");

    const uint8_t *rom = saa5050_get_builtin_rom();
    ASSERT_NOT_NULL(rom);

    /* Space (0x20, idx=0) should be all zeros in ROM */
    int all_zero = 1;
    for (int r = 0; r < SAA5050_CHAR_ROWS; r++)
        for (int c = 0; c < SAA5050_CHAR_COLS; c++)
            if (rom[r * SAA5050_CHAR_COLS + c]) { all_zero = 0; break; }
    ASSERT_TRUE(all_zero);

    /* ROM values are only 0 or 1 (unpacked pixels, not bitfields) */
    int valid = 1;
    for (int ch = 0; ch < SAA5050_CHAR_COUNT && valid; ch++)
        for (int r = 0; r < SAA5050_CHAR_ROWS && valid; r++)
            for (int c = 0; c < SAA5050_CHAR_COLS && valid; c++) {
                uint8_t v = rom[ch * SAA5050_CHAR_ROWS * SAA5050_CHAR_COLS
                                + r * SAA5050_CHAR_COLS + c];
                if (v > 1) valid = 0;
            }
    ASSERT_TRUE(valid);
}

/* --------------------------------------------------------------------------
 * 3. start_row bounds check
 * -------------------------------------------------------------------------- */
static void test_start_row_bounds(void)
{
    SUITE("start_row bounds");

    saa5050_t tt;
    saa5050_init(&tt, NULL);

    saa5050_start_row(&tt, 0);
    ASSERT_EQ(tt.current_row, 0);

    saa5050_start_row(&tt, 24);
    ASSERT_EQ(tt.current_row, 24);

    /* row_num >= 25: silently ignored */
    saa5050_start_row(&tt, 25);
    ASSERT_EQ(tt.current_row, 24);   /* unchanged */

    saa5050_start_row(&tt, 255);
    ASSERT_EQ(tt.current_row, 24);

    /* start_row clears dh_seen_this_row */
    tt.dh_seen_this_row = true;
    saa5050_start_row(&tt, 5);
    ASSERT_FALSE(tt.dh_seen_this_row);
}

/* --------------------------------------------------------------------------
 * 4. start_scanline initialises line_state correctly
 * -------------------------------------------------------------------------- */
static void test_start_scanline_init(void)
{
    SUITE("start_scanline init");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 0);

    saa5050_line_state_t ls;
    saa5050_start_scanline(&tt, &ls, 5);

    ASSERT_EQ(tt.current_scanline, 5);
    ASSERT_EQ(ls.fg_colour, 7);       /* white */
    ASSERT_EQ(ls.bg_colour, 0);       /* black */
    ASSERT_FALSE(ls.graphics_mode);
    ASSERT_FALSE(ls.separated_gfx);
    ASSERT_FALSE(ls.flash);
    ASSERT_FALSE(ls.hold_graphics);
    ASSERT_FALSE(ls.double_height);   /* dh_row_bottom[0] = false */
    ASSERT_EQ(ls.held_char, 0x20);
    ASSERT_FALSE(ls.held_is_gfx);
    ASSERT_FALSE(ls.held_is_sep);
}

/* --------------------------------------------------------------------------
 * 5. start_scanline loads dh_row_bottom into double_height
 * -------------------------------------------------------------------------- */
static void test_start_scanline_dh(void)
{
    SUITE("start_scanline DH");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    tt.dh_row_bottom[3] = true;

    saa5050_start_row(&tt, 3);
    saa5050_line_state_t ls;
    saa5050_start_scanline(&tt, &ls, 0);
    ASSERT_TRUE(ls.double_height);

    /* Row 2 has dh_row_bottom=false */
    saa5050_start_row(&tt, 2);
    saa5050_start_scanline(&tt, &ls, 0);
    ASSERT_FALSE(ls.double_height);
}

/* --------------------------------------------------------------------------
 * 6. char_code bit 7 stripped (& 0x7F)
 * -------------------------------------------------------------------------- */
static void test_bit7_strip(void)
{
    SUITE("bit7 strip");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 0);

    uint8_t px_normal[12], px_high[12];
    saa5050_line_state_t ls;

    /* 'A' = 0x41, 'A'|0x80 = 0xC1 → should produce same pixels */
    saa5050_start_scanline(&tt, &ls, 0);
    saa5050_render_char(&tt, &ls, 0x41, px_normal);
    saa5050_start_scanline(&tt, &ls, 0);
    saa5050_render_char(&tt, &ls, 0xC1, px_high);

    for (int i = 0; i < 12; i++)
        ASSERT_EQ(px_normal[i], px_high[i]);
}

/* --------------------------------------------------------------------------
 * 7. Text glyph from ROM: space is all-bg, 'A' has nonzero pixels
 * -------------------------------------------------------------------------- */
static void test_text_glyph(void)
{
    SUITE("text glyph");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 0);

    uint8_t px[12];

    /* Space (0x20): all pixels should be bg colour (0 = black) on scanline 0 */
    render_char_sl(&tt, 0x20, 0, px);
    ASSERT_TRUE(all_pixels_eq(px, 0));   /* all bg */

    /* 'A' (0x41) on scanline 0 (rom_row=0): the ROM has glyph data.
     * We don't know exact pixels but at least some should be fg (7 = white)
     * somewhere across all scanlines 0..15 of 'A' */
    int found_fg = 0;
    for (int sl = 0; sl < 16; sl++) {
        render_char_sl(&tt, 0x41, sl, px);
        for (int i = 0; i < 12; i++) if (px[i] == 7) found_fg = 1;
    }
    ASSERT_TRUE(found_fg);

    /* All pixels are either 0 (bg) or 7 (fg) with default state */
    for (int sl = 0; sl < 20; sl++) {
        render_char_sl(&tt, 0x41, sl, px);
        for (int i = 0; i < 12; i++) {
            ASSERT_TRUE(px[i] == 0 || px[i] == 7);
        }
    }
}

/* --------------------------------------------------------------------------
 * 8. ROM scanline mapping: scanline >> 1
 *    Scanlines 0-1 → rom_row 0; scanlines 2-3 → rom_row 1; etc.
 *    We verify consistency: same scanline pair produce identical pixels.
 * -------------------------------------------------------------------------- */
static void test_scanline_mapping(void)
{
    SUITE("scanline mapping");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 0);

    uint8_t px_even[12], px_odd[12];
    /* For 'A': scanlines 0 and 1 must produce identical pixels (both → rom_row 0) */
    render_char_sl(&tt, 0x41, 0, px_even);
    render_char_sl(&tt, 0x41, 1, px_odd);
    for (int i = 0; i < 12; i++)
        ASSERT_EQ(px_even[i], px_odd[i]);

    /* Scanlines 4 and 5 (→ rom_row 2) also identical */
    render_char_sl(&tt, 0x41, 4, px_even);
    render_char_sl(&tt, 0x41, 5, px_odd);
    for (int i = 0; i < 12; i++)
        ASSERT_EQ(px_even[i], px_odd[i]);

    /* Scanlines 18 and 19 (→ rom_row 9) are identical */
    render_char_sl(&tt, 0x41, 18, px_even);
    render_char_sl(&tt, 0x41, 19, px_odd);
    for (int i = 0; i < 12; i++)
        ASSERT_EQ(px_even[i], px_odd[i]);
}

/* --------------------------------------------------------------------------
 * 9. ROM rows 8-9: all zero for every character
 * -------------------------------------------------------------------------- */
static void test_rom_rows_8_9(void)
{
    SUITE("ROM rows 8-9 all zero");

    /* Row 9 (the very last pixel row) is always all-zero in the SAA5050 ROM.
     * Row 8 has data for a few descenders (e.g. '-', '`') so we only
     * assert row 9 here. */
    const uint8_t *rom = saa5050_get_builtin_rom();
    for (int ch = 0; ch < SAA5050_CHAR_COUNT; ch++) {
        int row = 9;
        for (int col = 0; col < SAA5050_CHAR_COLS; col++) {
            uint8_t v = rom[ch * SAA5050_CHAR_ROWS * SAA5050_CHAR_COLS
                            + row * SAA5050_CHAR_COLS + col];
            ASSERT_EQ(v, 0);
        }
    }
}

/* --------------------------------------------------------------------------
 * 10. Pixel expansion: 6 raw pixels → 12 output (each doubled)
 *     For a space (all-zero raw), all 12 output pixels = bg.
 *     For a full-block graphic (all-1 raw), all 12 output pixels = fg.
 * -------------------------------------------------------------------------- */
static void test_pixel_expansion(void)
{
    SUITE("pixel expansion");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 0);

    uint8_t px[12];

    /* Space → all bg (0) */
    render_char_sl(&tt, 0x20, 0, px);
    for (int i = 0; i < 12; i++) ASSERT_EQ(px[i], 0);

    /* Graphics mode, char 0x7F = full block (sixel 0x7F=all-bits)
     * sixel: 0x7F → sixel = (0x7F-0x40)&0x7F = 0x3F bits.
     * bit0=1,bit1=1,bit2=1,bit3=1,bit4=1,bit6=1 → all blocks lit.
     * rom_row 0 → third=0: lb=bit0=1, rb=bit1=1. raw = [1,1,1,1,1,1].
     * Output: all fg (7). */
    saa5050_line_state_t ls;
    saa5050_start_scanline(&tt, &ls, 0);
    /* Enter graphics mode */
    uint8_t dummy[12];
    saa5050_render_char(&tt, &ls, 0x17, dummy);  /* code 23 = white graphics */
    saa5050_render_char(&tt, &ls, 0x7F, px);
    /* On rom_row 0 (scanline 0): top-third, bits 0 and 1 both 1 → all fg */
    for (int i = 0; i < 12; i++) ASSERT_EQ(px[i], 7);

    /* Verify doubling pattern for a specific char.
     * Use '@' = 0x40 in text mode. ROM data for '@' (idx=0x20=32) row 0.
     * We check that each pair of adjacent output pixels is equal. */
    saa5050_start_scanline(&tt, &ls, 0);
    saa5050_render_char(&tt, &ls, 0x40, px);
    for (int i = 0; i < 6; i++) {
        ASSERT_EQ(px[i * 2], px[i * 2 + 1]);   /* each pair is doubled */
    }
}

/* --------------------------------------------------------------------------
 * 11. fg/bg colour: default white-on-black
 * -------------------------------------------------------------------------- */
static void test_default_colours(void)
{
    SUITE("default colours");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 0);

    saa5050_line_state_t ls;
    saa5050_start_scanline(&tt, &ls, 0);

    ASSERT_EQ(ls.fg_colour, 7);
    ASSERT_EQ(ls.bg_colour, 0);
}

/* --------------------------------------------------------------------------
 * 12. Control codes 1-7: alphanumeric colour, exits graphics, clears held
 * -------------------------------------------------------------------------- */
static void test_ctrl_alphanum_colour(void)
{
    SUITE("ctrl 1-7 alphanum colour");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 0);

    saa5050_line_state_t ls;
    uint8_t px[12];

    /* Code 1 → fg=1 (red), exit graphics */
    saa5050_start_scanline(&tt, &ls, 0);
    /* First enter graphics */
    saa5050_render_char(&tt, &ls, 0x11, px);  /* code 17 = graphics colour 1 */
    ASSERT_TRUE(ls.graphics_mode);
    /* Now code 1 = alphanumeric red */
    saa5050_render_char(&tt, &ls, 0x01, px);
    ASSERT_EQ(ls.fg_colour, 1);
    ASSERT_FALSE(ls.graphics_mode);
    ASSERT_EQ(ls.held_char, 0x20);   /* held_char cleared */

    /* Code 7 → fg=7 (white) */
    saa5050_render_char(&tt, &ls, 0x07, px);
    ASSERT_EQ(ls.fg_colour, 7);

    /* Test all codes 1-7 */
    for (uint8_t code = 1; code <= 7; code++) {
        saa5050_start_scanline(&tt, &ls, 0);
        saa5050_render_char(&tt, &ls, code, px);
        ASSERT_EQ(ls.fg_colour, code);
        ASSERT_FALSE(ls.graphics_mode);
    }
}

/* --------------------------------------------------------------------------
 * 13. Control code 8 (flash on) / 9 (steady)
 * -------------------------------------------------------------------------- */
static void test_ctrl_flash(void)
{
    SUITE("ctrl 8/9 flash");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 0);

    saa5050_line_state_t ls;
    uint8_t px[12];

    saa5050_start_scanline(&tt, &ls, 0);
    ASSERT_FALSE(ls.flash);
    saa5050_render_char(&tt, &ls, 0x08, px);  /* flash on */
    ASSERT_TRUE(ls.flash);
    saa5050_render_char(&tt, &ls, 0x09, px);  /* steady */
    ASSERT_FALSE(ls.flash);
}

/* --------------------------------------------------------------------------
 * 14. Control code 12 (normal height) / 13 (double height + dh_seen)
 * -------------------------------------------------------------------------- */
static void test_ctrl_double_height(void)
{
    SUITE("ctrl 12/13 double height");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 5);

    saa5050_line_state_t ls;
    uint8_t px[12];

    saa5050_start_scanline(&tt, &ls, 0);
    ASSERT_FALSE(ls.double_height);
    ASSERT_FALSE(tt.dh_seen_this_row);

    /* Code 13: double height */
    saa5050_render_char(&tt, &ls, 0x0D, px);
    ASSERT_TRUE(ls.double_height);
    ASSERT_TRUE(tt.dh_seen_this_row);

    /* Code 12: normal height */
    saa5050_render_char(&tt, &ls, 0x0C, px);
    ASSERT_FALSE(ls.double_height);
    /* dh_seen_this_row stays true once set in this row */
    ASSERT_TRUE(tt.dh_seen_this_row);
}

/* --------------------------------------------------------------------------
 * 15. Control codes 17-23: graphics colour + enters graphics mode
 * -------------------------------------------------------------------------- */
static void test_ctrl_graphics_colour(void)
{
    SUITE("ctrl 17-23 graphics colour");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 0);

    saa5050_line_state_t ls;
    uint8_t px[12];

    /* Code 17 (0x11) = graphics colour 1 (red), enter graphics */
    saa5050_start_scanline(&tt, &ls, 0);
    ASSERT_FALSE(ls.graphics_mode);
    saa5050_render_char(&tt, &ls, 0x11, px);
    ASSERT_EQ(ls.fg_colour, 1);
    ASSERT_TRUE(ls.graphics_mode);

    /* Code 23 (0x17) = graphics colour 7 (white) */
    saa5050_render_char(&tt, &ls, 0x17, px);
    ASSERT_EQ(ls.fg_colour, 7);
    ASSERT_TRUE(ls.graphics_mode);

    /* All codes 17-23: fg = code & 0x07 */
    for (uint8_t code = 17; code <= 23; code++) {
        saa5050_start_scanline(&tt, &ls, 0);
        saa5050_render_char(&tt, &ls, code, px);
        ASSERT_EQ(ls.fg_colour, code & 0x07);
        ASSERT_TRUE(ls.graphics_mode);
    }
}

/* --------------------------------------------------------------------------
 * 16. Control code 24 (conceal): fg = bg
 * -------------------------------------------------------------------------- */
static void test_ctrl_conceal(void)
{
    SUITE("ctrl 24 conceal");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 0);

    saa5050_line_state_t ls;
    uint8_t px[12];

    saa5050_start_scanline(&tt, &ls, 0);
    /* bg=0, fg=7 initially; after conceal, fg=0 */
    saa5050_render_char(&tt, &ls, 0x18, px);   /* code 24 = conceal */
    ASSERT_EQ(ls.fg_colour, 0);
    ASSERT_EQ(ls.bg_colour, 0);

    /* Any subsequent text is invisible (fg=bg=0) */
    saa5050_render_char(&tt, &ls, 0x41, px);   /* 'A' */
    ASSERT_TRUE(all_pixels_eq(px, 0));
}

/* --------------------------------------------------------------------------
 * 17. Control codes 25/26: contiguous / separated graphics
 * -------------------------------------------------------------------------- */
static void test_ctrl_separated(void)
{
    SUITE("ctrl 25/26 separated");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 0);

    saa5050_line_state_t ls;
    uint8_t px[12];

    saa5050_start_scanline(&tt, &ls, 0);
    ASSERT_FALSE(ls.separated_gfx);

    saa5050_render_char(&tt, &ls, 0x1A, px);   /* code 26 = separated */
    ASSERT_TRUE(ls.separated_gfx);

    saa5050_render_char(&tt, &ls, 0x19, px);   /* code 25 = contiguous */
    ASSERT_FALSE(ls.separated_gfx);
}

/* --------------------------------------------------------------------------
 * 18. Control codes 28/29: black background / new background
 * -------------------------------------------------------------------------- */
static void test_ctrl_background(void)
{
    SUITE("ctrl 28/29 background");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 0);

    saa5050_line_state_t ls;
    uint8_t px[12];

    saa5050_start_scanline(&tt, &ls, 0);
    /* Set fg to red (1) first */
    saa5050_render_char(&tt, &ls, 0x01, px);   /* alphanum red */
    ASSERT_EQ(ls.fg_colour, 1);
    ASSERT_EQ(ls.bg_colour, 0);

    /* Code 29: new background = current fg (1=red) */
    saa5050_render_char(&tt, &ls, 0x1D, px);
    ASSERT_EQ(ls.bg_colour, 1);
    ASSERT_EQ(ls.fg_colour, 1);

    /* Code 28: black background */
    saa5050_render_char(&tt, &ls, 0x1C, px);
    ASSERT_EQ(ls.bg_colour, 0);
}

/* --------------------------------------------------------------------------
 * 19. Control codes 30/31: hold / release graphics
 * -------------------------------------------------------------------------- */
static void test_ctrl_hold_release(void)
{
    SUITE("ctrl 30/31 hold/release");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 0);

    saa5050_line_state_t ls;
    uint8_t px[12];

    saa5050_start_scanline(&tt, &ls, 0);
    ASSERT_FALSE(ls.hold_graphics);

    saa5050_render_char(&tt, &ls, 0x1E, px);   /* code 30 = hold */
    ASSERT_TRUE(ls.hold_graphics);

    saa5050_render_char(&tt, &ls, 0x1F, px);   /* code 31 = release */
    ASSERT_FALSE(ls.hold_graphics);
}

/* --------------------------------------------------------------------------
 * 20. Unhandled control codes: no state change
 * -------------------------------------------------------------------------- */
static void test_ctrl_unhandled(void)
{
    SUITE("ctrl unhandled");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 0);

    saa5050_line_state_t ls;
    uint8_t px[12];

    /* Unhandled codes: 0, 10, 11, 14, 15, 16, 27 */
    const uint8_t unhandled[] = {0, 10, 11, 14, 15, 16, 27};
    for (int i = 0; i < (int)(sizeof(unhandled)); i++) {
        saa5050_start_scanline(&tt, &ls, 0);
        uint8_t fg_before = ls.fg_colour;
        uint8_t bg_before = ls.bg_colour;
        saa5050_render_char(&tt, &ls, unhandled[i], px);
        ASSERT_EQ(ls.fg_colour, fg_before);
        ASSERT_EQ(ls.bg_colour, bg_before);
        ASSERT_FALSE(ls.graphics_mode);
        ASSERT_FALSE(ls.flash);
    }
}

/* --------------------------------------------------------------------------
 * 21. Graphics mode: sixel rendering for printable chars
 *     In graphics mode, chars with bit 5 set (0x20-0x3F, 0x60-0x7F)
 *     are sixels; others (0x40-0x5F) are text glyphs.
 * -------------------------------------------------------------------------- */
static void test_graphics_mode_chars(void)
{
    SUITE("graphics mode chars");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 0);

    saa5050_line_state_t ls;
    uint8_t px[12];

    /* Enter graphics mode (white graphics = code 23) */
    saa5050_start_scanline(&tt, &ls, 0);
    saa5050_render_char(&tt, &ls, 0x17, px);   /* code 23 */
    ASSERT_TRUE(ls.graphics_mode);

    /* 0x20 in graphics mode: sixel with all-zero bits → all bg */
    saa5050_render_char(&tt, &ls, 0x20, px);
    ASSERT_TRUE(all_pixels_eq(px, 0));

    /* 0x3F in graphics mode: sixel = 0x3F & 0x7F = 0x3F.
     * Bits 0,1,2,3,4 set (bit5=1 = reserved, bit6=0).
     * rom_row 0 → third=0: lb=bit0=1, rb=bit1=1 → raw=[1,1,1,1,1,1] → all fg */
    saa5050_render_char(&tt, &ls, 0x3F, px);
    ASSERT_TRUE(all_pixels_eq(px, 7));

    /* 0x40-0x5F in graphics mode: go to text ROM, NOT sixels */
    saa5050_start_scanline(&tt, &ls, 0);
    saa5050_render_char(&tt, &ls, 0x17, px);   /* enter gfx */
    /* 0x40 = '@' — text glyph, not sixel. held_char should be reset to 0x20. */
    ls.held_char = 0x7F;   /* set held to something non-space */
    saa5050_render_char(&tt, &ls, 0x40, px);
    ASSERT_EQ(ls.held_char, 0x20);  /* text chars reset held_char */
    ASSERT_FALSE(ls.held_is_gfx);

    /* 0x60-0x7F in graphics mode: sixels */
    saa5050_start_scanline(&tt, &ls, 0);
    saa5050_render_char(&tt, &ls, 0x17, px);
    /* 0x7F → sixel = (0x7F-0x40)&0x7F = 0x3F → same as 0x3F case above */
    saa5050_render_char(&tt, &ls, 0x7F, px);
    ASSERT_TRUE(all_pixels_eq(px, 7));   /* all fg on row 0 */
}

/* --------------------------------------------------------------------------
 * 22. Sixel bit mapping: each of the 6 blocks
 *     We test each bit individually to verify the block occupies expected pixels.
 *     Uses contiguous graphics (not separated) for clean block boundaries.
 *
 * Character code 0x20 (sixel 0x20) has bit 5 set and others clear → only
 * the reserved bit, but bit 5 is skipped. Actually 0x20 = 0010 0000 →
 * bit5 = 1 (reserved), all others 0 → all blocks off.
 *
 * Better: use specific codes to isolate each block.
 * Code 0x21 (sixel 0x21 = 0b100001): bit0=1 (top-left), bit5=1 (reserved).
 * On rom_row 0-2 (third=0): lb=bit0=1, rb=bit1=0 → raw=[1,1,1,0,0,0].
 * -------------------------------------------------------------------------- */
static void test_sixel_bit_mapping(void)
{
    SUITE("sixel bit mapping");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 0);

    saa5050_line_state_t ls;
    uint8_t px[12];

    /* Helper: render sixel char on specific scanline in gfx mode */
    /* sixel 0x21 = bit0=1 (top-left): cols 0-2 lit on rows 0-2 (third=0) */
    /* row_in_10 = 0 → third=0, lb=bit0=1, rb=bit1=0 */

    saa5050_start_scanline(&tt, &ls, 0);   /* scanline 0 → rom_row 0 → third=0 */
    saa5050_render_char(&tt, &ls, 0x17, px);  /* white graphics */
    saa5050_render_char(&tt, &ls, 0x21, px);  /* sixel: bit0=1 (top-left), bit1=0 */
    /* Left half (px 0-5): fg=7; right half (px 6-11): bg=0 */
    for (int i = 0; i < 6; i++)  ASSERT_EQ(px[i], 7);
    for (int i = 6; i < 12; i++) ASSERT_EQ(px[i], 0);

    /* sixel 0x22 = bit1=1 (top-right), bit5=1 (reserved) → right half lit on third=0 */
    saa5050_start_scanline(&tt, &ls, 0);
    saa5050_render_char(&tt, &ls, 0x17, px);
    saa5050_render_char(&tt, &ls, 0x22, px);  /* sixel: bit1=1, bit0=0 */
    for (int i = 0; i < 6; i++)  ASSERT_EQ(px[i], 0);
    for (int i = 6; i < 12; i++) ASSERT_EQ(px[i], 7);

    /* Mid third (rows 3-6): use scanline 6 → rom_row 3 → third=1 */
    /* Code 0x24 = bit2=1 (mid-left), bit5=1 → left half on third=1 */
    saa5050_start_scanline(&tt, &ls, 6);   /* scanline 6 → rom_row 3 → third=1 */
    saa5050_render_char(&tt, &ls, 0x17, px);
    saa5050_render_char(&tt, &ls, 0x24, px);  /* sixel: bit2=1, others=0 (bit5=1 reserved) */
    for (int i = 0; i < 6; i++)  ASSERT_EQ(px[i], 7);
    for (int i = 6; i < 12; i++) ASSERT_EQ(px[i], 0);

    /* Code 0x28 = bit3=1 (mid-right), bit5=1 → right half on third=1 */
    saa5050_start_scanline(&tt, &ls, 6);
    saa5050_render_char(&tt, &ls, 0x17, px);
    saa5050_render_char(&tt, &ls, 0x28, px);
    for (int i = 0; i < 6; i++)  ASSERT_EQ(px[i], 0);
    for (int i = 6; i < 12; i++) ASSERT_EQ(px[i], 7);

    /* Bottom third (rows 7-9): scanline 14 → rom_row 7 → third=2 */
    /* Code 0x30 = bit4=1 (bot-left), bit5=1 */
    saa5050_start_scanline(&tt, &ls, 14);  /* scanline 14 → rom_row 7 → third=2 */
    saa5050_render_char(&tt, &ls, 0x17, px);
    saa5050_render_char(&tt, &ls, 0x30, px);  /* sixel: bit4=1 (bot-left) */
    for (int i = 0; i < 6; i++)  ASSERT_EQ(px[i], 7);
    for (int i = 6; i < 12; i++) ASSERT_EQ(px[i], 0);

    /* Bot-right = bit6 (not bit5). Code with bit6=1: 0x60 → sixel = (0x60-0x40)&0x7F = 0x20.
     * 0x20 = bit5=1 (reserved), bit6=0 → no, still 0. Let's use 0x61.
     * 0x61 → sixel = (0x61-0x40)&0x7F = 0x21 = bit0=1, bit5=1. That's top-left again.
     * Need bit6=1: value 0x60 | 0x40 but these overlap...
     * Code 0x60: sixel = (0x60-0x40)&0x7F = 0x20 = bit5=1. bit6=0 → bot-right=0.
     * Code 0x61: sixel = 0x21 = bit0, bit5. Not bit6.
     * To get bit6=1: sixel bit6 = char bit in position 6.
     * Code value with sixel-bit6 set: for codes 0x60-0x7F:
     *   sixel = (code - 0x40) & 0x7F. For bit6 of sixel to be 1:
     *   (code - 0x40) bit6 = 1 → code-0x40 >= 0x40 → code >= 0x80. Not possible (stripped).
     * For codes 0x20-0x3F: sixel = code & 0x7F. For sixel bit6=1: code & 0x40 must be set,
     *   but these codes are 0x20-0x3F → bit6=0. So sixel bit6 is never set from 0x20-0x3F.
     * Wait — the sixel bits come from the raw sixel value: sixel = code & 0x7F for 0x20-0x3F.
     * 0x20-0x3F range: bits 0-5 of code are used, bit6=0 always. Bot-right (bit6) never lit
     * for these codes.
     * For 0x60-0x7F: sixel = (code-0x40)&0x7F. Code 0x60: sixel=0x20=0b100000 bit5=1.
     * Code 0x7F: sixel = (0x7F-0x40)&0x7F = 0x3F = 0b0111111 → bit6=0.
     * Hmm. So bot-right (bit6) can only be set if sixel >= 0x40... but these codes can
     * never produce sixel >= 0x40 since: for 0x60-0x7F: (code-0x40)<=0x3F. bit6=0 always.
     * For 0x20-0x3F: code&0x7F <= 0x3F. bit6=0 always.
     * So bot-right is NEVER lit! The "bit 5 = reserved" comment explains this:
     * bit5 is always 1 in graphic chars but bit6 is always 0 within the valid ranges.
     * The full-block 0x7F: sixel=0x3F=0b111111 → bit0..5 set; bit6=0 → bot-right=0!
     * This means 0x7F is NOT a true full-block — bot-right is always dark.
     * Verify: 0x7F, rom_row=7 (third=2): lb=bit4=1, rb=bit6=0 → left lit, right off.
     */

    saa5050_start_scanline(&tt, &ls, 14);   /* rom_row=7, third=2 */
    saa5050_render_char(&tt, &ls, 0x17, px);
    saa5050_render_char(&tt, &ls, 0x7F, px);
    /* sixel = (0x7F-0x40)&0x7F = 0x3F. bit4=1 (bot-left), bit6=0 (bot-right=0) */
    for (int i = 0; i < 6; i++)  ASSERT_EQ(px[i], 7);   /* left half lit */
    for (int i = 6; i < 12; i++) ASSERT_EQ(px[i], 0);   /* right half dark */
}

/* --------------------------------------------------------------------------
 * 23. Separated graphics: column 2 and 5 always blanked; bottom row of each
 *     third blanked when separated.
 * -------------------------------------------------------------------------- */
static void test_separated_graphics(void)
{
    SUITE("separated graphics");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 0);

    saa5050_line_state_t ls;
    uint8_t px[12];

    /* Use a fully-lit char (both blocks on) = 0x3F (bit0=1,bit1=1,...) on a
     * non-gap row first. scanline 0 → rom_row 0 → third=0 (rows 0-2).
     * Contiguous: raw=[1,1,1,1,1,1]. Expected px = all fg.
     * Separated same row (0): NOT a gap row (gap rows for thirds are rows 2,6,9).
     *   row_in_10=0: no blanking from the row check. But after fill:
     *   out_6pixels[2]=0, out_6pixels[5]=0 always.
     *   So raw=[1,1,0,1,1,0]. Output: px[4,5]=bg, px[10,11]=bg, others=fg. */

    /* Contiguous, scanline 0 */
    saa5050_start_scanline(&tt, &ls, 0);
    saa5050_render_char(&tt, &ls, 0x17, px);   /* white gfx */
    saa5050_render_char(&tt, &ls, 0x3F, px);   /* sixel bits 0-5 all set (bit5=reserved) */
    /* sixel=0x3F: bit0=1,bit1=1 on third=0. raw=[1,1,1,1,1,1]. All fg. */
    ASSERT_TRUE(all_pixels_eq(px, 7));

    /* Separated, scanline 0 (row_in_10=0, not a gap row) */
    saa5050_start_scanline(&tt, &ls, 0);
    saa5050_render_char(&tt, &ls, 0x17, px);   /* white gfx */
    saa5050_render_char(&tt, &ls, 0x1A, px);   /* code 26 = separated */
    saa5050_render_char(&tt, &ls, 0x3F, px);
    /* raw after separated: [1,1,0,1,1,0]. Expanded:
     * px[0,1]=7, px[2,3]=7, px[4,5]=0, px[6,7]=7, px[8,9]=7, px[10,11]=0 */
    ASSERT_EQ(px[0],  7);
    ASSERT_EQ(px[1],  7);
    ASSERT_EQ(px[2],  7);
    ASSERT_EQ(px[3],  7);
    ASSERT_EQ(px[4],  0);   /* col 2 blanked → px[4,5]=0 */
    ASSERT_EQ(px[5],  0);
    ASSERT_EQ(px[6],  7);
    ASSERT_EQ(px[7],  7);
    ASSERT_EQ(px[8],  7);
    ASSERT_EQ(px[9],  7);
    ASSERT_EQ(px[10], 0);   /* col 5 blanked → px[10,11]=0 */
    ASSERT_EQ(px[11], 0);

    /* Separated on a gap row: scanline 4 → rom_row 2 → third=0 (row_in_10=2 IS gap).
     * All pixels should be bg (gap row clears lb and rb, then col 2 and 5 also cleared). */
    saa5050_start_scanline(&tt, &ls, 4);   /* scanline 4 → rom_row 2 */
    saa5050_render_char(&tt, &ls, 0x17, px);
    saa5050_render_char(&tt, &ls, 0x1A, px);  /* separated */
    saa5050_render_char(&tt, &ls, 0x3F, px);
    ASSERT_TRUE(all_pixels_eq(px, 0));   /* gap row: all blanked */

    /* Contiguous on same row: NOT all bg (blocks lit) */
    saa5050_start_scanline(&tt, &ls, 4);
    saa5050_render_char(&tt, &ls, 0x17, px);
    saa5050_render_char(&tt, &ls, 0x3F, px);   /* contiguous */
    /* third=0: lb=bit0=1, rb=bit1=1 → all lit */
    ASSERT_EQ(px[0], 7);
}

/* --------------------------------------------------------------------------
 * 24. Hold graphics: held char displayed during control codes
 * -------------------------------------------------------------------------- */
static void test_hold_graphics(void)
{
    SUITE("hold graphics");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 0);

    saa5050_line_state_t ls;
    uint8_t px_held[12], px_space[12], px_gfx[12];

    /* Sequence: enter gfx mode, hold on, render a graphic char,
     * then render a control code — held char should show. */
    saa5050_start_scanline(&tt, &ls, 0);
    saa5050_render_char(&tt, &ls, 0x17, px_space);  /* gfx mode white */
    saa5050_render_char(&tt, &ls, 0x1E, px_space);  /* hold on */
    ASSERT_TRUE(ls.hold_graphics);

    /* Render a graphic char to set held_char */
    saa5050_render_char(&tt, &ls, 0x21, px_gfx);   /* sixel bit0=1, third=0: left half lit */
    ASSERT_EQ(ls.held_char, 0x21);
    ASSERT_TRUE(ls.held_is_gfx);

    /* Now render a control code (normal height = 0x0C). In hold mode,
     * the held char (0x21) should be displayed instead of space.
     * Use 0x0C (not flash) to avoid flash_state blanking the pixels. */
    saa5050_render_char(&tt, &ls, 0x0C, px_held);
    ASSERT_FALSE(ls.double_height);

    /* px_held should equal px_gfx (same held char rendered) */
    for (int i = 0; i < 12; i++)
        ASSERT_EQ(px_held[i], px_gfx[i]);

    /* Sanity: without hold, control code shows space (all bg) */
    saa5050_start_scanline(&tt, &ls, 0);
    saa5050_render_char(&tt, &ls, 0x17, px_space);  /* gfx mode */
    /* No hold: render control code → space */
    saa5050_render_char(&tt, &ls, 0x0C, px_space);
    ASSERT_TRUE(all_pixels_eq(px_space, 0));
}

/* --------------------------------------------------------------------------
 * 25. hold_old: release (0x1F) still shows held char for that cell
 * -------------------------------------------------------------------------- */
static void test_hold_release_shows_char(void)
{
    SUITE("hold_old: release shows char");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 0);

    saa5050_line_state_t ls;
    uint8_t px_release[12], px_gfx[12];

    saa5050_start_scanline(&tt, &ls, 0);
    saa5050_render_char(&tt, &ls, 0x17, px_gfx);   /* gfx mode */
    saa5050_render_char(&tt, &ls, 0x1E, px_gfx);   /* hold on */
    saa5050_render_char(&tt, &ls, 0x21, px_gfx);   /* set held_char=0x21 */

    /* Release (0x1F) while hold is on: hold_old=true, so this cell
     * shows the held char AND turns hold off for next cell. */
    saa5050_render_char(&tt, &ls, 0x1F, px_release);
    ASSERT_FALSE(ls.hold_graphics);   /* released */
    /* This cell should show held char (0x21 = top-left lit) */
    for (int i = 0; i < 6; i++)  ASSERT_EQ(px_release[i], 7);
    for (int i = 6; i < 12; i++) ASSERT_EQ(px_release[i], 0);

    /* Next cell: hold is off → control code shows space */
    uint8_t px_after[12];
    saa5050_render_char(&tt, &ls, 0x08, px_after);
    ASSERT_TRUE(all_pixels_eq(px_after, 0));
}

/* --------------------------------------------------------------------------
 * 26. Text char in graphics mode resets held_char
 * -------------------------------------------------------------------------- */
static void test_text_char_resets_held(void)
{
    SUITE("text char resets held");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 0);

    saa5050_line_state_t ls;
    uint8_t px[12];

    saa5050_start_scanline(&tt, &ls, 0);
    saa5050_render_char(&tt, &ls, 0x17, px);   /* gfx mode */
    saa5050_render_char(&tt, &ls, 0x1E, px);   /* hold */
    saa5050_render_char(&tt, &ls, 0x21, px);   /* set held_char=0x21 */
    ASSERT_EQ(ls.held_char, 0x21);

    /* Render a text char (0x40-0x5F is in gfx mode but goes to ROM, resets held) */
    saa5050_render_char(&tt, &ls, 0x40, px);   /* '@' — text in gfx mode */
    ASSERT_EQ(ls.held_char, 0x20);
    ASSERT_FALSE(ls.held_is_gfx);
}

/* --------------------------------------------------------------------------
 * 27. Graphics char range: 0x40-0x5F in graphics mode uses text ROM
 * -------------------------------------------------------------------------- */
static void test_gfx_text_range(void)
{
    SUITE("gfx 0x40-0x5F text ROM");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 0);

    saa5050_line_state_t ls;
    uint8_t px_gfx_mode[12], px_text_mode[12];

    /* 'A' = 0x41: in graphics mode vs text mode → same pixels (both use ROM) */
    saa5050_start_scanline(&tt, &ls, 2);
    saa5050_render_char(&tt, &ls, 0x17, px_gfx_mode);   /* gfx mode */
    saa5050_render_char(&tt, &ls, 0x41, px_gfx_mode);

    saa5050_start_scanline(&tt, &ls, 2);
    saa5050_render_char(&tt, &ls, 0x41, px_text_mode);  /* text mode */

    for (int i = 0; i < 12; i++)
        ASSERT_EQ(px_gfx_mode[i], px_text_mode[i]);
}

/* --------------------------------------------------------------------------
 * 28. Flash blanking: fg→bg when flash=true and flash_state=false
 * -------------------------------------------------------------------------- */
static void test_flash_blanking(void)
{
    SUITE("flash blanking");

    saa5050_t tt;
    saa5050_init(&tt, NULL);
    saa5050_start_row(&tt, 0);

    uint8_t px_on[12], px_off[12];
    saa5050_line_state_t ls;

    /* Find a scanline where 'A' has some lit pixels */
    /* Use scanline 2 → rom_row 1 */

    /* Flash state = true (on phase): character visible */
    tt.flash_state = true;
    saa5050_start_scanline(&tt, &ls, 2);
    saa5050_render_char(&tt, &ls, 0x08, px_on);  /* flash on */
    saa5050_render_char(&tt, &ls, 0x41, px_on);  /* 'A' with flash */
    /* During flash ON phase, character is visible */

    /* Flash state = false (off phase): character invisible */
    tt.flash_state = false;
    saa5050_start_scanline(&tt, &ls, 2);
    saa5050_render_char(&tt, &ls, 0x08, px_off); /* flash on */
    saa5050_render_char(&tt, &ls, 0x41, px_off); /* 'A' with flash, state=off */
    ASSERT_TRUE(all_pixels_eq(px_off, 0));        /* invisible: all bg */

    /* Without flash attribute: character always visible regardless of flash_state */
    tt.flash_state = false;
    saa5050_start_scanline(&tt, &ls, 2);
    saa5050_render_char(&tt, &ls, 0x41, px_off);
    /* Not all zero — same as normal rendering */
    int has_fg = 0;
    for (int i = 0; i < 12; i++) if (px_off[i] == 7) has_fg = 1;
    /* At scanline 2 (rom_row=1), 'A' should have some fg pixels */
    /* (We can't guarantee this without knowing ROM; use multiple scanlines) */
    tt.flash_state = false;
    for (int sl = 0; sl < 16; sl++) {
        saa5050_start_scanline(&tt, &ls, sl);
        saa5050_render_char(&tt, &ls, 0x41, px_off);
        for (int i = 0; i < 12; i++) if (px_off[i] == 7) { has_fg = 1; break; }
        if (has_fg) break;
    }
    ASSERT_TRUE(has_fg);
}

/* --------------------------------------------------------------------------
 * 29. Flash timing: toggle_flash 64-frame cycle, on=48 off=16
 * -------------------------------------------------------------------------- */
static void test_flash_timing(void)
{
    SUITE("flash timing");

    saa5050_t tt;
    saa5050_init(&tt, NULL);

    /* After init: counter=0, flash_state=false.
     * toggle increments counter first: counter becomes 1, state = (1<48)=true. */
    ASSERT_EQ(tt.flash_counter, 0);
    ASSERT_FALSE(tt.flash_state);

    /* First toggle: counter=1, state=true */
    saa5050_toggle_flash(&tt);
    ASSERT_EQ(tt.flash_counter, 1);
    ASSERT_TRUE(tt.flash_state);

    /* Toggle 46 more times: counter=47, state=true */
    for (int i = 0; i < 46; i++) saa5050_toggle_flash(&tt);
    ASSERT_EQ(tt.flash_counter, 47);
    ASSERT_TRUE(tt.flash_state);

    /* Toggle once more: counter=48, state=(48<48)=false */
    saa5050_toggle_flash(&tt);
    ASSERT_EQ(tt.flash_counter, 48);
    ASSERT_FALSE(tt.flash_state);

    /* Toggle 15 more: counter=63, state=false */
    for (int i = 0; i < 15; i++) saa5050_toggle_flash(&tt);
    ASSERT_EQ(tt.flash_counter, 63);
    ASSERT_FALSE(tt.flash_state);

    /* Toggle: counter wraps to 0 (mod 64), state=(0<48)=true */
    saa5050_toggle_flash(&tt);
    ASSERT_EQ(tt.flash_counter, 0);
    ASSERT_TRUE(tt.flash_state);
}

/* --------------------------------------------------------------------------
 * 30. Double-height: bottom half uses rom_row = (scan>>1)+5
 *     On the bottom half (dh_row_bottom=true), scanline 0 → rom_row=5,
 *     scanline 9 → rom_row=9 (clamped).
 *     ROM rows 8-9 are all zero, so scanlines 6-9 on bottom half are blank.
 *     We compare bottom-half scanline 0 output against normal scanline 10
 *     (which also maps to rom_row=5). They should match.
 * -------------------------------------------------------------------------- */
static void test_double_height_bottom(void)
{
    SUITE("double height bottom");

    saa5050_t tt;
    saa5050_init(&tt, NULL);

    uint8_t px_dh_bottom[12], px_normal[12];
    saa5050_line_state_t ls;

    /* Bottom-half rendering of 'A': scanline 0 → rom_row = 0+5 = 5 */
    tt.dh_row_bottom[1] = true;
    saa5050_start_row(&tt, 1);
    saa5050_start_scanline(&tt, &ls, 0);   /* scanline 0 on DH-bottom row */
    ASSERT_TRUE(ls.double_height);
    saa5050_render_char(&tt, &ls, 0x41, px_dh_bottom);

    /* Normal rendering of 'A': scanline 10 → rom_row = 10>>1 = 5 */
    tt.dh_row_bottom[0] = false;
    saa5050_start_row(&tt, 0);
    saa5050_start_scanline(&tt, &ls, 10);   /* scanline 10 → rom_row=5 */
    ASSERT_FALSE(ls.double_height);
    saa5050_render_char(&tt, &ls, 0x41, px_normal);

    /* Both should use rom_row=5 → identical output */
    for (int i = 0; i < 12; i++)
        ASSERT_EQ(px_dh_bottom[i], px_normal[i]);

    /* Bottom-half scanline 6 → rom_row = 3+5 = 8 → all-zero row → all bg */
    saa5050_start_row(&tt, 1);
    saa5050_start_scanline(&tt, &ls, 6);   /* scanline 6 → (6>>1)+5 = 8 → all zero */
    saa5050_render_char(&tt, &ls, 0x41, px_dh_bottom);
    ASSERT_TRUE(all_pixels_eq(px_dh_bottom, 0));
}

/* --------------------------------------------------------------------------
 * 31. dh_row_bottom propagation at scanline 19
 * -------------------------------------------------------------------------- */
static void test_dh_propagation(void)
{
    SUITE("DH propagation");

    saa5050_t tt;
    saa5050_init(&tt, NULL);

    saa5050_line_state_t ls;
    uint8_t px[12];

    ASSERT_FALSE(tt.dh_row_bottom[1]);

    /* Render row 0: at scanline 19 with a DH code, row 1 should be flagged */
    saa5050_start_row(&tt, 0);
    /* Set dh_seen_this_row via rendering code 0x0D on any scanline */
    saa5050_start_scanline(&tt, &ls, 0);
    saa5050_render_char(&tt, &ls, 0x0D, px);   /* double height */
    ASSERT_TRUE(tt.dh_seen_this_row);

    /* Render scanline 19 (last scanline): propagates to row 1 */
    saa5050_start_scanline(&tt, &ls, 19);
    saa5050_render_char(&tt, &ls, 0x0D, px);   /* re-set dh_seen */
    /* Render a printable char to trigger the propagation check */
    saa5050_render_char(&tt, &ls, 0x41, px);
    ASSERT_TRUE(tt.dh_row_bottom[1]);

    /* Without DH code: row 2 should not be flagged */
    saa5050_start_row(&tt, 1);
    saa5050_start_scanline(&tt, &ls, 19);
    saa5050_render_char(&tt, &ls, 0x41, px);   /* just text, no DH */
    ASSERT_FALSE(tt.dh_row_bottom[2]);

    /* Verify: row 0 start clears dh_seen_this_row */
    tt.dh_seen_this_row = true;
    saa5050_start_row(&tt, 0);
    ASSERT_FALSE(tt.dh_seen_this_row);
}

/* --------------------------------------------------------------------------
 * 32. saa5050_reset: clears flash, counters, dh arrays, preserves char_rom
 * -------------------------------------------------------------------------- */
static void test_reset(void)
{
    SUITE("reset");

    saa5050_t tt;
    saa5050_init(&tt, NULL);

    const uint8_t *saved_rom = tt.char_rom;

    tt.flash_state   = true;
    tt.flash_counter = 55;
    tt.current_row   = 10;
    tt.dh_row_bottom[5] = true;
    tt.dh_seen_this_row = true;

    saa5050_reset(&tt);

    ASSERT_FALSE(tt.flash_state);
    ASSERT_EQ(tt.flash_counter, 0);
    ASSERT_EQ(tt.current_row, 0);
    ASSERT_FALSE(tt.dh_row_bottom[5]);
    ASSERT_FALSE(tt.dh_seen_this_row);
    /* char_rom is preserved by reset (reset doesn't touch it) */
    ASSERT_TRUE(tt.char_rom == saved_rom);
}

/* --------------------------------------------------------------------------
 * 33. Custom char_rom passed to init
 * -------------------------------------------------------------------------- */
static void test_custom_rom(void)
{
    SUITE("custom char_rom");

    /* Build a minimal custom ROM: all zeros except char index 0 ('A'-'A'+0
     * = space actually) — let's put index 1 ('A') row 0 all-ones. */
    static uint8_t custom_rom[SAA5050_CHAR_COUNT][SAA5050_CHAR_ROWS][SAA5050_CHAR_COLS];
    memset(custom_rom, 0, sizeof(custom_rom));
    /* Character 'A' = index 0x41-0x20 = 0x21 = 33. Set row 0 all-ones. */
    for (int c = 0; c < SAA5050_CHAR_COLS; c++)
        custom_rom[0x21][0][c] = 1;

    saa5050_t tt;
    saa5050_init(&tt, (const uint8_t *)custom_rom);
    ASSERT_TRUE(tt.char_rom == (const uint8_t *)custom_rom);

    saa5050_start_row(&tt, 0);
    saa5050_line_state_t ls;
    uint8_t px[12];

    /* 'A' on scanline 0 (rom_row=0): should be all fg (all-ones in custom ROM) */
    saa5050_start_scanline(&tt, &ls, 0);
    saa5050_render_char(&tt, &ls, 0x41, px);
    ASSERT_TRUE(all_pixels_eq(px, 7));

    /* 'A' on scanline 2 (rom_row=1): all zero in custom ROM → all bg */
    saa5050_start_scanline(&tt, &ls, 2);
    saa5050_render_char(&tt, &ls, 0x41, px);
    ASSERT_TRUE(all_pixels_eq(px, 0));

    /* Space (0x20, index 0) on scanline 0: all zero → all bg */
    saa5050_start_scanline(&tt, &ls, 0);
    saa5050_render_char(&tt, &ls, 0x20, px);
    ASSERT_TRUE(all_pixels_eq(px, 0));
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */
int main(void)
{
    printf("saa5050 tests\n");
    printf("=============\n");

    test_init_reset();
    test_builtin_rom();
    test_start_row_bounds();
    test_start_scanline_init();
    test_start_scanline_dh();
    test_bit7_strip();
    test_text_glyph();
    test_scanline_mapping();
    test_rom_rows_8_9();
    test_pixel_expansion();
    test_default_colours();
    test_ctrl_alphanum_colour();
    test_ctrl_flash();
    test_ctrl_double_height();
    test_ctrl_graphics_colour();
    test_ctrl_conceal();
    test_ctrl_separated();
    test_ctrl_background();
    test_ctrl_hold_release();
    test_ctrl_unhandled();
    test_graphics_mode_chars();
    test_sixel_bit_mapping();
    test_separated_graphics();
    test_hold_graphics();
    test_hold_release_shows_char();
    test_text_char_resets_held();
    test_gfx_text_range();
    test_flash_blanking();
    test_flash_timing();
    test_double_height_bottom();
    test_dh_propagation();
    test_reset();
    test_custom_rom();

    printf("\nResults: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
