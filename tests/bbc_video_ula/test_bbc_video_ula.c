/*
 * Tests for bbc_video_ula — Acorn Video ULA emulation
 *
 * Covers:
 *   - init/reset state
 *   - control register decoding (bpp mode, teletext, 2MHz, flash)
 *   - palette write: logical→physical mapping, XOR-inversion quirk
 *   - bit interleaving: 1bpp, 2bpp, 4bpp (the BBC-specific layout)
 *   - serialize: pixel count, correct colours, cursor inversion
 *   - flash toggle: palette flip, tables rebuilt
 *   - colour table: physical colour → RGB
 */

#include "bbc_video_ula.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Minimal test framework
 * -------------------------------------------------------------------------- */
static int s_pass = 0, s_fail = 0;
static const char *s_current_test = "";

#define TEST(name) do { s_current_test = (name); } while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("  FAIL [%s] line %d: expected %d, got %d\n", \
               s_current_test, __LINE__, (int)(b), (int)(a)); \
        s_fail++; \
    } else { s_pass++; } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        printf("  FAIL [%s] line %d: expected true\n", s_current_test, __LINE__); \
        s_fail++; \
    } else { s_pass++; } \
} while(0)

#define ASSERT_FALSE(x) do { \
    if (x) { \
        printf("  FAIL [%s] line %d: expected false\n", s_current_test, __LINE__); \
        s_fail++; \
    } else { s_pass++; } \
} while(0)

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

/* Write palette entry: logical colour lc maps to physical colour pc.
 * The ULA write format: bits[7:4] = logical, bits[2:0] = physical XOR 7 */
static void write_palette(bbc_video_ula_t *ula, uint8_t lc, uint8_t pc)
{
    /* physical bits are stored as (pc ^ 7), so we write (pc ^ 7) in bits[2:0] */
    uint8_t data = ((lc & 0x0F) << 4) | ((pc ^ 7) & 0x07);
    bbc_video_ula_write(ula, 1, data);   /* addr=1 → &FE21 */
}

/* Set control register */
static void write_ctrl(bbc_video_ula_t *ula, uint8_t data)
{
    bbc_video_ula_write(ula, 0, data);   /* addr=0 → &FE20 */
}

/* --------------------------------------------------------------------------
 * 1. init / reset state
 * -------------------------------------------------------------------------- */
static void test_init_state(void)
{
    TEST("init: teletext_mode false");
    bbc_video_ula_t ula;
    bbc_video_ula_init(&ula);
    ASSERT_FALSE(ula.teletext_mode);

    TEST("init: bpp_mode = 1bpp");
    ASSERT_EQ(ula.bpp_mode, ULA_BPP_1);

    TEST("init: pixels_per_byte = 8");
    ASSERT_EQ(ula.pixels_per_byte, 8);

    TEST("init: crtc_2mhz false");
    ASSERT_FALSE(ula.crtc_2mhz);

    TEST("init: flash_state false");
    ASSERT_FALSE(ula.flash_state);

    /* Default palette: logical N → physical N (for 0-7) */
    TEST("init: palette[0] = BLACK");
    ASSERT_EQ(ula.palette[0], BBC_COL_BLACK);

    TEST("init: palette[1] = RED");
    ASSERT_EQ(ula.palette[1], BBC_COL_RED);

    TEST("init: palette[7] = WHITE");
    ASSERT_EQ(ula.palette[7], BBC_COL_WHITE);
}

/* --------------------------------------------------------------------------
 * 2. Control register decoding
 * -------------------------------------------------------------------------- */
static void test_control_register(void)
{
    bbc_video_ula_t ula;
    bbc_video_ula_init(&ula);

    /* Bit 1: teletext mode */
    TEST("ctrl: teletext bit set");
    write_ctrl(&ula, 0x02);
    ASSERT_TRUE(ula.teletext_mode);

    TEST("ctrl: teletext bit clear");
    write_ctrl(&ula, 0x00);
    ASSERT_FALSE(ula.teletext_mode);

    /* Bit 4: CRTC 2MHz */
    TEST("ctrl: crtc_2mhz bit set");
    write_ctrl(&ula, 0x10);
    ASSERT_TRUE(ula.crtc_2mhz);

    TEST("ctrl: crtc_2mhz bit clear");
    write_ctrl(&ula, 0x00);
    ASSERT_FALSE(ula.crtc_2mhz);

    /* Bit 0: flash_state */
    TEST("ctrl: flash_state bit set");
    write_ctrl(&ula, 0x01);
    ASSERT_TRUE(ula.flash_state);

    TEST("ctrl: flash_state bit clear");
    write_ctrl(&ula, 0x00);
    ASSERT_FALSE(ula.flash_state);

    /* Bits 3-2: bpp mode */
    TEST("ctrl: bpp=00 → ULA_BPP_1, 8px/byte");
    write_ctrl(&ula, 0x00);
    ASSERT_EQ(ula.bpp_mode, ULA_BPP_1);
    ASSERT_EQ(ula.pixels_per_byte, 8);

    TEST("ctrl: bpp=01 → ULA_BPP_2, 4px/byte");
    write_ctrl(&ula, 0x04);
    ASSERT_EQ(ula.bpp_mode, ULA_BPP_2);
    ASSERT_EQ(ula.pixels_per_byte, 4);

    TEST("ctrl: bpp=10 → ULA_BPP_4, 2px/byte");
    write_ctrl(&ula, 0x08);
    ASSERT_EQ(ula.bpp_mode, ULA_BPP_4);
    ASSERT_EQ(ula.pixels_per_byte, 2);

    TEST("ctrl: bpp=11 → ULA_BPP_8, 1px/byte");
    write_ctrl(&ula, 0x0C);
    ASSERT_EQ(ula.bpp_mode, ULA_BPP_8);
    ASSERT_EQ(ula.pixels_per_byte, 1);
}

/* --------------------------------------------------------------------------
 * 3. Palette write: XOR-inversion quirk
 * -------------------------------------------------------------------------- */
static void test_palette_write(void)
{
    bbc_video_ula_t ula;
    bbc_video_ula_init(&ula);

    /* Writing physical colour 0 (BLACK) → bits[2:0] = 0 XOR 7 = 7 */
    TEST("palette: logical 0 → physical BLACK");
    write_palette(&ula, 0, BBC_COL_BLACK);
    ASSERT_EQ(ula.palette[0], BBC_COL_BLACK);

    /* Writing physical colour 7 (WHITE) → bits[2:0] = 7 XOR 7 = 0 */
    TEST("palette: logical 0 → physical WHITE");
    write_palette(&ula, 0, BBC_COL_WHITE);
    ASSERT_EQ(ula.palette[0], BBC_COL_WHITE);

    /* Writing physical colour 1 (RED) → bits[2:0] = 1 XOR 7 = 6 */
    TEST("palette: logical 3 → physical RED");
    write_palette(&ula, 3, BBC_COL_RED);
    ASSERT_EQ(ula.palette[3], BBC_COL_RED);

    /* XOR quirk directly: raw write data 0x3F → logical=3, raw_phys=0xF&7=7,
     * stored as 7 XOR 7 = 0 = BLACK */
    TEST("palette: XOR quirk raw write 0x3F → logical 3 = BLACK");
    bbc_video_ula_write(&ula, 1, 0x3F);
    ASSERT_EQ(ula.palette[3], BBC_COL_BLACK);

    /* raw write 0x38 → logical=3, raw_phys=8&7=0, stored as 0^7=7=WHITE */
    TEST("palette: XOR quirk raw write 0x38 → logical 3 = WHITE");
    bbc_video_ula_write(&ula, 1, 0x38);
    ASSERT_EQ(ula.palette[3], BBC_COL_WHITE);

    /* All 16 logical entries writable */
    TEST("palette: logical 15 writable");
    write_palette(&ula, 15, BBC_COL_CYAN);
    ASSERT_EQ(ula.palette[15], BBC_COL_CYAN);
}

/* --------------------------------------------------------------------------
 * 4. 1bpp bit interleaving
 *
 * Pixel i = bit (7-i) of the byte.
 * With default palette (logical N → physical N):
 *   0xFF → all pixels = 1 = RED (with default palette)
 *   0x00 → all pixels = 0 = BLACK
 *   0x80 → pixel 0 = 1 (RED), pixels 1-7 = 0 (BLACK)
 *   0x01 → pixel 7 = 1 (RED), pixels 0-6 = 0 (BLACK)
 *   0xAA → pixels 0,2,4,6 = 1 (RED), 1,3,5,7 = 0 (BLACK)
 * -------------------------------------------------------------------------- */
static void test_1bpp_interleaving(void)
{
    bbc_video_ula_t ula;
    bbc_video_ula_init(&ula);
    write_ctrl(&ula, 0x00);   /* 1bpp, no teletext */
    uint8_t px[8];

    TEST("1bpp: 0xFF → 8 pixels all = 1 (RED)");
    bbc_video_ula_serialize(&ula, 0xFF, px, false);
    for (int i = 0; i < 8; i++) ASSERT_EQ(px[i], BBC_COL_RED);   /* logical 1 = RED */

    TEST("1bpp: 0x00 → 8 pixels all = 0 (BLACK)");
    bbc_video_ula_serialize(&ula, 0x00, px, false);
    for (int i = 0; i < 8; i++) ASSERT_EQ(px[i], BBC_COL_BLACK);

    TEST("1bpp: 0x80 → pixel 0=RED, rest=BLACK");
    bbc_video_ula_serialize(&ula, 0x80, px, false);
    ASSERT_EQ(px[0], BBC_COL_RED);
    for (int i = 1; i < 8; i++) ASSERT_EQ(px[i], BBC_COL_BLACK);

    TEST("1bpp: 0x01 → pixel 7=RED, rest=BLACK");
    bbc_video_ula_serialize(&ula, 0x01, px, false);
    ASSERT_EQ(px[7], BBC_COL_RED);
    for (int i = 0; i < 7; i++) ASSERT_EQ(px[i], BBC_COL_BLACK);

    /* 0xAA = 1010 1010: bits 7,5,3,1 = 1 → pixels 0,2,4,6 = 1 (RED) */
    TEST("1bpp: 0xAA → pixels 0,2,4,6 = RED, 1,3,5,7 = BLACK");
    bbc_video_ula_serialize(&ula, 0xAA, px, false);
    ASSERT_EQ(px[0], BBC_COL_RED);
    ASSERT_EQ(px[1], BBC_COL_BLACK);
    ASSERT_EQ(px[2], BBC_COL_RED);
    ASSERT_EQ(px[3], BBC_COL_BLACK);
    ASSERT_EQ(px[4], BBC_COL_RED);
    ASSERT_EQ(px[5], BBC_COL_BLACK);
    ASSERT_EQ(px[6], BBC_COL_RED);
    ASSERT_EQ(px[7], BBC_COL_BLACK);

    /* 0x55 = 0101 0101: bits 6,4,2,0 = 1 → pixels 1,3,5,7 = 1 (RED) */
    TEST("1bpp: 0x55 → pixels 1,3,5,7 = RED, 0,2,4,6 = BLACK");
    bbc_video_ula_serialize(&ula, 0x55, px, false);
    ASSERT_EQ(px[0], BBC_COL_BLACK);
    ASSERT_EQ(px[1], BBC_COL_RED);
    ASSERT_EQ(px[2], BBC_COL_BLACK);
    ASSERT_EQ(px[3], BBC_COL_RED);
    ASSERT_EQ(px[4], BBC_COL_BLACK);
    ASSERT_EQ(px[5], BBC_COL_RED);
    ASSERT_EQ(px[6], BBC_COL_BLACK);
    ASSERT_EQ(px[7], BBC_COL_RED);

    TEST("1bpp: serialize returns 8");
    int n = bbc_video_ula_serialize(&ula, 0x00, px, false);
    ASSERT_EQ(n, 8);
}

/* --------------------------------------------------------------------------
 * 5. 2bpp bit interleaving
 *
 * Pixel i: high bit = bit(7-i), low bit = bit(3-i). logical = (high<<1)|low.
 * 4 pixels per byte. With default palette:
 *
 * Byte 0xFF = 1111 1111:
 *   px0: high=bit7=1, low=bit3=1 → logical 3 → YELLOW
 *   px1: high=bit6=1, low=bit2=1 → logical 3 → YELLOW
 *   px2: high=bit5=1, low=bit1=1 → logical 3 → YELLOW
 *   px3: high=bit4=1, low=bit0=1 → logical 3 → YELLOW
 *
 * Byte 0x00: all logical 0 → BLACK
 *
 * Byte 0xF0 = 1111 0000:
 *   px0: high=bit7=1, low=bit3=0 → logical 2 → GREEN
 *   px1: high=bit6=1, low=bit2=0 → logical 2 → GREEN
 *   px2: high=bit5=1, low=bit1=0 → logical 2 → GREEN
 *   px3: high=bit4=1, low=bit0=0 → logical 2 → GREEN
 *
 * Byte 0x0F = 0000 1111:
 *   px0: high=0, low=1 → logical 1 → RED
 *   all px = RED
 *
 * Byte 0x80 = 1000 0000:
 *   px0: high=bit7=1, low=bit3=0 → logical 2 → GREEN
 *   px1: high=bit6=0, low=bit2=0 → logical 0 → BLACK
 *   px2: high=bit5=0, low=bit1=0 → logical 0 → BLACK
 *   px3: high=bit4=0, low=bit0=0 → logical 0 → BLACK
 *
 * Byte 0x08 = 0000 1000:
 *   px0: high=bit7=0, low=bit3=1 → logical 1 → RED
 *   px1,2,3: all 0 → BLACK
 * -------------------------------------------------------------------------- */
static void test_2bpp_interleaving(void)
{
    bbc_video_ula_t ula;
    bbc_video_ula_init(&ula);
    write_ctrl(&ula, 0x04);   /* 2bpp */
    uint8_t px[4];

    TEST("2bpp: 0xFF → 4 pixels all YELLOW (logical 3)");
    bbc_video_ula_serialize(&ula, 0xFF, px, false);
    for (int i = 0; i < 4; i++) ASSERT_EQ(px[i], BBC_COL_YELLOW);

    TEST("2bpp: 0x00 → 4 pixels all BLACK");
    bbc_video_ula_serialize(&ula, 0x00, px, false);
    for (int i = 0; i < 4; i++) ASSERT_EQ(px[i], BBC_COL_BLACK);

    TEST("2bpp: 0xF0 → 4 pixels all GREEN (logical 2, high=1 low=0)");
    bbc_video_ula_serialize(&ula, 0xF0, px, false);
    for (int i = 0; i < 4; i++) ASSERT_EQ(px[i], BBC_COL_GREEN);

    TEST("2bpp: 0x0F → 4 pixels all RED (logical 1, high=0 low=1)");
    bbc_video_ula_serialize(&ula, 0x0F, px, false);
    for (int i = 0; i < 4; i++) ASSERT_EQ(px[i], BBC_COL_RED);

    TEST("2bpp: 0x80 → px0=GREEN, px1-3=BLACK");
    bbc_video_ula_serialize(&ula, 0x80, px, false);
    ASSERT_EQ(px[0], BBC_COL_GREEN);
    ASSERT_EQ(px[1], BBC_COL_BLACK);
    ASSERT_EQ(px[2], BBC_COL_BLACK);
    ASSERT_EQ(px[3], BBC_COL_BLACK);

    TEST("2bpp: 0x08 → px0=RED, px1-3=BLACK");
    bbc_video_ula_serialize(&ula, 0x08, px, false);
    ASSERT_EQ(px[0], BBC_COL_RED);
    ASSERT_EQ(px[1], BBC_COL_BLACK);
    ASSERT_EQ(px[2], BBC_COL_BLACK);
    ASSERT_EQ(px[3], BBC_COL_BLACK);

    /* Isolate pixel 3: high=bit4, low=bit0 */
    TEST("2bpp: 0x11 → px3=YELLOW, rest=BLACK");
    /* 0x11 = 0001 0001: bit4=1 → high of px3=1; bit0=1 → low of px3=1 → logical 3 */
    bbc_video_ula_serialize(&ula, 0x11, px, false);
    ASSERT_EQ(px[0], BBC_COL_BLACK);
    ASSERT_EQ(px[1], BBC_COL_BLACK);
    ASSERT_EQ(px[2], BBC_COL_BLACK);
    ASSERT_EQ(px[3], BBC_COL_YELLOW);

    TEST("2bpp: serialize returns 4");
    int n = bbc_video_ula_serialize(&ula, 0x00, px, false);
    ASSERT_EQ(n, 4);
}

/* --------------------------------------------------------------------------
 * 6. 4bpp bit interleaving
 *
 * Pixel 0: bits {7,5,3,1} → b3,b2,b1,b0 → logical (b3<<3)|(b2<<2)|(b1<<1)|b0
 * Pixel 1: bits {6,4,2,0}
 * 2 pixels per byte. With default palette.
 *
 * Byte 0xFF:
 *   px0: {1,1,1,1} → logical 15 → WHITE
 *   px1: {1,1,1,1} → logical 15 → WHITE
 *
 * Byte 0x00: both BLACK (logical 0)
 *
 * Byte 0xAA = 1010 1010:
 *   px0: bit7=1,bit5=0,bit3=1,bit2→bit1=0 → {1,0,1,0} → logical 10 → CYAN? No wait.
 *   Let's compute carefully:
 *   0xAA = 1010 1010
 *   px0: b3=bit7=1, b2=bit5=0, b1=bit3=1, b0=bit1=0 → logical = 8+0+2+0 = 10
 *   px1: b3=bit6=0, b2=bit4=1, b1=bit2=0, b0=bit0=1 → logical = 0+4+0+1 = 5 → MAGENTA
 *
 * Byte 0x55 = 0101 0101:
 *   px0: bit7=0,bit5=1,bit3=0,bit1=1 → logical = 0+4+0+1 = 5 → MAGENTA
 *   px1: bit6=1,bit4=0,bit2=1,bit0=0 → logical = 8+0+2+0 = 10 → depends on palette[10]
 *
 * Simpler cases:
 * Byte 0x80 = 1000 0000:
 *   px0: b3=bit7=1, b2=bit5=0, b1=bit3=0, b0=bit1=0 → logical 8 → default palette[8] = BLACK
 *   px1: b3=bit6=0, ... = logical 0 → BLACK
 *   Both BLACK with default palette since palette[8]=BLACK
 *
 * Let's map each bit to a specific logical colour:
 * Byte with only bit7=1: px0 gets bit7→b3=1 → logical 8 (palette[8]=BLACK by default)
 * That's tricky with default palette. Use a custom palette instead.
 *
 * Better approach: set all 16 palette entries to distinct colours, then verify
 * that specific bytes produce the expected logical colour indices.
 * -------------------------------------------------------------------------- */
static void test_4bpp_interleaving(void)
{
    bbc_video_ula_t ula;
    bbc_video_ula_init(&ula);
    write_ctrl(&ula, 0x08);   /* 4bpp */
    uint8_t px[2];

    /* Map all 16 logical colours to themselves (default palette already does
     * logical 0-7 → physical 0-7; but 8-15 also → 0-7 in default.
     * Override so logical N → physical (N & 7) for easy verification. */
    /* Actually default palette already has palette[8-15] = 0-7, so
     * logical 10 → physical 2 (GREEN), logical 5 → physical 5 (MAGENTA). */

    TEST("4bpp: 0xFF → 2 pixels both WHITE (logical 15 → palette[15]=WHITE)");
    bbc_video_ula_serialize(&ula, 0xFF, px, false);
    ASSERT_EQ(px[0], BBC_COL_WHITE);
    ASSERT_EQ(px[1], BBC_COL_WHITE);

    TEST("4bpp: 0x00 → 2 pixels both BLACK");
    bbc_video_ula_serialize(&ula, 0x00, px, false);
    ASSERT_EQ(px[0], BBC_COL_BLACK);
    ASSERT_EQ(px[1], BBC_COL_BLACK);

    /* Byte 0x02 = 0000 0010:
     * px0: b3=bit7=0, b2=bit5=0, b1=bit3=0, b0=bit1=1 → logical 1 → RED
     * px1: b3=bit6=0, b2=bit4=0, b1=bit2=0, b0=bit0=0 → logical 0 → BLACK */
    TEST("4bpp: 0x02 → px0=RED (logical 1), px1=BLACK (logical 0)");
    bbc_video_ula_serialize(&ula, 0x02, px, false);
    ASSERT_EQ(px[0], BBC_COL_RED);
    ASSERT_EQ(px[1], BBC_COL_BLACK);

    /* Byte 0x01 = 0000 0001:
     * px1: b0=bit0=1 → logical 1 → RED
     * px0: all 0 → logical 0 → BLACK */
    TEST("4bpp: 0x01 → px0=BLACK, px1=RED");
    bbc_video_ula_serialize(&ula, 0x01, px, false);
    ASSERT_EQ(px[0], BBC_COL_BLACK);
    ASSERT_EQ(px[1], BBC_COL_RED);

    /* Byte 0x20 = 0010 0000:
     * px0: b2=bit5=1 → logical 4 → BLUE
     * px1: all 0 → BLACK */
    TEST("4bpp: 0x20 → px0=BLUE (logical 4), px1=BLACK");
    bbc_video_ula_serialize(&ula, 0x20, px, false);
    ASSERT_EQ(px[0], BBC_COL_BLUE);
    ASSERT_EQ(px[1], BBC_COL_BLACK);

    /* Byte 0x10 = 0001 0000:
     * px1: b2=bit4=1 → logical 4 → BLUE
     * px0: 0 → BLACK */
    TEST("4bpp: 0x10 → px0=BLACK, px1=BLUE");
    bbc_video_ula_serialize(&ula, 0x10, px, false);
    ASSERT_EQ(px[0], BBC_COL_BLACK);
    ASSERT_EQ(px[1], BBC_COL_BLUE);

    /* Byte 0x08 = 0000 1000:
     * px0: b1=bit3=1 → logical 2 → GREEN */
    TEST("4bpp: 0x08 → px0=GREEN (logical 2), px1=BLACK");
    bbc_video_ula_serialize(&ula, 0x08, px, false);
    ASSERT_EQ(px[0], BBC_COL_GREEN);
    ASSERT_EQ(px[1], BBC_COL_BLACK);

    /* Byte 0x04 = 0000 0100:
     * px1: b1=bit2=1 → logical 2 → GREEN */
    TEST("4bpp: 0x04 → px0=BLACK, px1=GREEN");
    bbc_video_ula_serialize(&ula, 0x04, px, false);
    ASSERT_EQ(px[0], BBC_COL_BLACK);
    ASSERT_EQ(px[1], BBC_COL_GREEN);

    /* Byte 0x80 = 1000 0000:
     * px0: b3=bit7=1 → logical 8 → palette[8]=BLACK (default)
     * px1: all 0 → BLACK */
    TEST("4bpp: 0x80 → px0=BLACK (logical 8 → palette[8]), px1=BLACK");
    bbc_video_ula_serialize(&ula, 0x80, px, false);
    ASSERT_EQ(px[0], BBC_COL_BLACK);   /* palette[8] = BLACK by default */
    ASSERT_EQ(px[1], BBC_COL_BLACK);

    /* Now map palette[8] = WHITE to distinguish */
    write_palette(&ula, 8, BBC_COL_WHITE);
    TEST("4bpp: 0x80 with palette[8]=WHITE → px0=WHITE");
    bbc_video_ula_serialize(&ula, 0x80, px, false);
    ASSERT_EQ(px[0], BBC_COL_WHITE);
    ASSERT_EQ(px[1], BBC_COL_BLACK);

    TEST("4bpp: serialize returns 2");
    int n = bbc_video_ula_serialize(&ula, 0x00, px, false);
    ASSERT_EQ(n, 2);
}

/* --------------------------------------------------------------------------
 * 7. Cursor inversion
 * colour XOR 7 for each pixel
 * -------------------------------------------------------------------------- */
static void test_cursor_inversion(void)
{
    bbc_video_ula_t ula;
    bbc_video_ula_init(&ula);
    write_ctrl(&ula, 0x00);   /* 1bpp */
    uint8_t px[8];

    /* 0x00 → all BLACK (0). With cursor: 0 XOR 7 = 7 = WHITE */
    TEST("cursor: 1bpp 0x00 no-cursor → BLACK");
    bbc_video_ula_serialize(&ula, 0x00, px, false);
    ASSERT_EQ(px[0], BBC_COL_BLACK);

    TEST("cursor: 1bpp 0x00 with-cursor → WHITE (0^7)");
    bbc_video_ula_serialize(&ula, 0x00, px, true);
    for (int i = 0; i < 8; i++) ASSERT_EQ(px[i], BBC_COL_WHITE);

    /* 0xFF → all RED (1 with default). With cursor: 1 XOR 7 = 6 = CYAN */
    TEST("cursor: 1bpp 0xFF with-cursor → CYAN (1^7)");
    bbc_video_ula_serialize(&ula, 0xFF, px, true);
    for (int i = 0; i < 8; i++) ASSERT_EQ(px[i], BBC_COL_CYAN);

    /* 2bpp, 0xFF → all YELLOW (3). With cursor: 3 XOR 7 = 4 = BLUE */
    write_ctrl(&ula, 0x04);
    uint8_t px4[4];
    TEST("cursor: 2bpp 0xFF with-cursor → BLUE (3^7)");
    bbc_video_ula_serialize(&ula, 0xFF, px4, true);
    for (int i = 0; i < 4; i++) ASSERT_EQ(px4[i], BBC_COL_BLUE);
}

/* --------------------------------------------------------------------------
 * 8. Flash toggle
 * -------------------------------------------------------------------------- */
static void test_flash_toggle(void)
{
    bbc_video_ula_t ula;
    bbc_video_ula_init(&ula);

    TEST("flash: initial state false");
    ASSERT_FALSE(ula.flash_state);

    TEST("flash: toggle → true");
    bbc_video_ula_toggle_flash(&ula);
    ASSERT_TRUE(ula.flash_state);

    TEST("flash: toggle → false");
    bbc_video_ula_toggle_flash(&ula);
    ASSERT_FALSE(ula.flash_state);

    /* Flash affects flashing colours (logical 8-15).
     * With default palette, flash_state does not change non-flash colours.
     * But if we use flash to switch colours: set logical 8 = WHITE when
     * flash_state=false, and logical 8 = BLACK when flash_state=true.
     * The ULA does this via the flash_state bit in the control register
     * selecting between palette[logical] and palette[logical^8].
     *
     * Actually in this implementation, flash_state is stored and the
     * rebuild_tables uses palette directly — there is no automatic
     * flash switching in the tables; the MOS writes two palette entries
     * and toggles flash_state to select. The toggle just flips flash_state
     * and rebuilds tables.
     * Verify the control register bit 0 tracks flash_state after toggle. */
    TEST("flash: control bit 0 set after toggle=true");
    bbc_video_ula_toggle_flash(&ula);   /* now true */
    ASSERT_TRUE((ula.control & ULA_CTRL_FLASH_STATE) != 0);

    TEST("flash: control bit 0 clear after toggle=false");
    bbc_video_ula_toggle_flash(&ula);   /* now false */
    ASSERT_TRUE((ula.control & ULA_CTRL_FLASH_STATE) == 0);
}

/* --------------------------------------------------------------------------
 * 9. Colour table: physical colour → RGB
 * -------------------------------------------------------------------------- */
static void test_colour_table(void)
{
    bbc_video_ula_t ula;
    bbc_video_ula_init(&ula);

    TEST("colour: BLACK = {0,0,0}");
    bbc_rgb_t c = bbc_video_ula_colour(&ula, BBC_COL_BLACK);
    ASSERT_EQ(c.r, 0); ASSERT_EQ(c.g, 0); ASSERT_EQ(c.b, 0);

    TEST("colour: RED = {255,0,0}");
    c = bbc_video_ula_colour(&ula, BBC_COL_RED);
    ASSERT_EQ(c.r, 255); ASSERT_EQ(c.g, 0); ASSERT_EQ(c.b, 0);

    TEST("colour: GREEN = {0,255,0}");
    c = bbc_video_ula_colour(&ula, BBC_COL_GREEN);
    ASSERT_EQ(c.r, 0); ASSERT_EQ(c.g, 255); ASSERT_EQ(c.b, 0);

    TEST("colour: YELLOW = {255,255,0}");
    c = bbc_video_ula_colour(&ula, BBC_COL_YELLOW);
    ASSERT_EQ(c.r, 255); ASSERT_EQ(c.g, 255); ASSERT_EQ(c.b, 0);

    TEST("colour: BLUE = {0,0,255}");
    c = bbc_video_ula_colour(&ula, BBC_COL_BLUE);
    ASSERT_EQ(c.r, 0); ASSERT_EQ(c.g, 0); ASSERT_EQ(c.b, 255);

    TEST("colour: MAGENTA = {255,0,255}");
    c = bbc_video_ula_colour(&ula, BBC_COL_MAGENTA);
    ASSERT_EQ(c.r, 255); ASSERT_EQ(c.g, 0); ASSERT_EQ(c.b, 255);

    TEST("colour: CYAN = {0,255,255}");
    c = bbc_video_ula_colour(&ula, BBC_COL_CYAN);
    ASSERT_EQ(c.r, 0); ASSERT_EQ(c.g, 255); ASSERT_EQ(c.b, 255);

    TEST("colour: WHITE = {255,255,255}");
    c = bbc_video_ula_colour(&ula, BBC_COL_WHITE);
    ASSERT_EQ(c.r, 255); ASSERT_EQ(c.g, 255); ASSERT_EQ(c.b, 255);
}

/* --------------------------------------------------------------------------
 * 10. Palette change rebuilds tables immediately
 * -------------------------------------------------------------------------- */
static void test_palette_rebuilds_tables(void)
{
    bbc_video_ula_t ula;
    bbc_video_ula_init(&ula);
    write_ctrl(&ula, 0x00);   /* 1bpp */
    uint8_t px[8];

    /* By default, logical 1 → RED. 0xFF → all RED. */
    TEST("tables: 0xFF → RED before palette change");
    bbc_video_ula_serialize(&ula, 0xFF, px, false);
    ASSERT_EQ(px[0], BBC_COL_RED);

    /* Remap logical 1 → CYAN */
    write_palette(&ula, 1, BBC_COL_CYAN);

    TEST("tables: 0xFF → CYAN after palette[1]=CYAN");
    bbc_video_ula_serialize(&ula, 0xFF, px, false);
    ASSERT_EQ(px[0], BBC_COL_CYAN);

    /* And 0x00 (logical 0 = BLACK) unchanged */
    TEST("tables: 0x00 → BLACK still (logical 0 unchanged)");
    bbc_video_ula_serialize(&ula, 0x00, px, false);
    ASSERT_EQ(px[0], BBC_COL_BLACK);
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */
int main(void)
{
    printf("bbc_video_ula tests\n");
    printf("===================\n\n");

    test_init_state();
    test_control_register();
    test_palette_write();
    test_1bpp_interleaving();
    test_2bpp_interleaving();
    test_4bpp_interleaving();
    test_cursor_inversion();
    test_flash_toggle();
    test_colour_table();
    test_palette_rebuilds_tables();

    printf("\nResults: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
