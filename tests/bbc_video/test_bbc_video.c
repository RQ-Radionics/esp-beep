/*
 * test_bbc_video.c — unit tests for bbc_video_render_row()
 *
 * Strategy: configure a bbc_video_t with synthetic register values, render a
 * full frame with bbc_video_render_frame() into a reference framebuffer, then
 * render each row individually with bbc_video_render_row() and verify the
 * output is pixel-identical row by row.
 *
 * Tests cover both bitmap modes and teletext (MODE 7).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "bbc_video.h"
#include "mc6845.h"
#include "bbc_video_ula.h"
#include "saa5050.h"

/* --------------------------------------------------------------------------
 * Minimal test framework
 * -------------------------------------------------------------------------- */
static int g_pass = 0;
static int g_fail = 0;
static const char *g_suite = "";

#define SUITE(name) do { g_suite = (name); printf("\n[%s]\n", name); } while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("  FAIL %s:%d: expected %d, got %d\n", \
               g_suite, __LINE__, (int)(b), (int)(a)); \
        g_fail++; \
    } else { g_pass++; } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        printf("  FAIL %s:%d: expected true\n", g_suite, __LINE__); \
        g_fail++; \
    } else { g_pass++; } \
} while(0)

/* --------------------------------------------------------------------------
 * Dimensions
 * -------------------------------------------------------------------------- */
#define OUT_W  BBC_FB_WIDTH    /* 640 */
#define OUT_H  BBC_FB_HEIGHT   /* 256 */

/* --------------------------------------------------------------------------
 * Helpers: set up MC6845 registers for a given mode
 * -------------------------------------------------------------------------- */

/* Write a full set of CRTC registers (R0-R17) */
static void crtc_load(mc6845_t *crtc, const uint8_t regs[18])
{
    for (int i = 0; i < 18; i++) {
        mc6845_write(crtc, 0, (uint8_t)i);   /* select register */
        mc6845_write(crtc, 1, regs[i]);       /* write value */
    }
}

/* MODE 0: 640×256, 2 MHz, 8 pixels/char, 32 cols × 32 rows × 8 scanlines */
static void setup_mode0(bbc_video_t *vid)
{
    /* CRTC registers for MODE 0:
     * R0=127 H-total, R1=80 H-disp, R2=98 H-sync, R3=0x28 sync widths,
     * R4=38 V-total, R5=0 V-adj, R6=32 V-disp, R7=35 V-sync,
     * R8=0 interlace, R9=7 max-scanline, R10=0x67 cursor-start, R11=8 cursor-end,
     * R12=0x30 start-H (addr=0x3000>>3=0x600 → MA start=0x600),
     * R13=0x00 start-L, R14-R17=0 */
    uint8_t regs[18] = {
        127, 80, 98, 0x28,  /* R0-R3 */
         38,  0, 32,   35,  /* R4-R7 */
          0,  7, 0x67,  8,  /* R8-R11 */
       0x30, 0x00, 0, 0,    /* R12-R15 */
          0,  0             /* R16-R17 */
    };
    crtc_load(&vid->crtc, regs);

    /* ULA: MODE 0 — 2 MHz, 8bpp, 2-colour palette */
    bbc_video_ula_write(&vid->ula, 0, 0x9C); /* CTRL: 2MHz=1, flash=1, teletext=0, pix_per_byte=8 bits[3:1]=111 → 3=8pp */
    /* palette: index 0=black(0), index 1=white(7) */
    bbc_video_ula_write(&vid->ula, 1, (0 << 4) | 0x00); /* col 0 → black */
    bbc_video_ula_write(&vid->ula, 1, (1 << 4) | 0x07); /* col 1 → white */
}

/* MODE 7 (teletext) */
static void setup_mode7(bbc_video_t *vid)
{
    /* CRTC registers for MODE 7 */
    uint8_t regs[18] = {
        63, 40, 51, 0x23,
         30, 2, 25,  27,
          0, 18, 0x72, 19,
       0x3C, 0x00, 0, 0,
          0,  0
    };
    crtc_load(&vid->crtc, regs);
    /* ULA: teletext mode */
    bbc_video_ula_write(&vid->ula, 0, 0x9F); /* teletext bit set */
}

/* --------------------------------------------------------------------------
 * Fill video RAM with a deterministic pattern
 * -------------------------------------------------------------------------- */
static void fill_ram_pattern(uint8_t *ram, uint32_t size)
{
    for (uint32_t i = 0; i < size; i++)
        ram[i] = (uint8_t)((i * 37 + 13) & 0xFF);
}

static void fill_ram_teletext(uint8_t *ram, uint32_t size)
{
    /* Fill MODE 7 region with printable ASCII + some control codes */
    memset(ram, 0x20, size);  /* space */
    uint32_t base = BBC_SCREEN_BASE_MODE7 & 0x7FFF;
    for (int row = 0; row < SAA5050_ROWS; row++) {
        for (int col = 0; col < SAA5050_COLS; col++) {
            uint32_t addr = base + (uint32_t)(row * SAA5050_COLS + col);
            if (addr < size) {
                /* Mix of printable chars and graphics */
                uint8_t c = (uint8_t)(0x20 + ((row * SAA5050_COLS + col) % 96));
                ram[addr] = c;
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * Core test: render_row must match render_frame row-by-row
 * -------------------------------------------------------------------------- */
static int test_row_matches_frame(bbc_video_t *vid, const char *mode_name)
{
    /* Allocate reference framebuffer (full frame) */
    uint8_t *ref_fb = (uint8_t *)calloc((size_t)(OUT_W * OUT_H), 1);
    if (!ref_fb) { printf("  OOM\n"); return -1; }

    /* Render full frame into ref_fb */
    bbc_video_output_t out = {
        .format      = BBC_FB_FORMAT_INDEX8,
        .width       = OUT_W,
        .height      = OUT_H,
        .framebuffer = ref_fb,
        .fb_stride   = OUT_W,
    };
    bbc_video_set_output(vid, &out);
    /* Reset SAA5050 state before reference render so both render_frame and
     * render_row start from the same initial state. */
    saa5050_reset(&vid->teletext);
    bbc_video_render_frame(vid);

    /* Now compare each row rendered individually.
     * Reset SAA5050 before each render_row call so state is fresh, matching
     * the state render_teletext_row reconstructs via its DH pre-pass. */
    uint8_t row_buf[OUT_W];
    int mismatches = 0;
    for (int y = 0; y < OUT_H; y++) {
        memset(row_buf, 0xAA, OUT_W);  /* sentinel */
        saa5050_reset(&vid->teletext);
        bbc_video_render_row(vid, y, OUT_H, row_buf, OUT_W);

        const uint8_t *ref_row = ref_fb + y * OUT_W;
        for (int x = 0; x < OUT_W; x++) {
            if (row_buf[x] != ref_row[x]) {
                if (mismatches < 5)
                    printf("  MISMATCH %s y=%d x=%d: row=%02x frame=%02x\n",
                           mode_name, y, x, row_buf[x], ref_row[x]);
                mismatches++;
            }
        }
    }

    free(ref_fb);
    return mismatches;
}

/* --------------------------------------------------------------------------
 * Test: null/invalid guards
 * -------------------------------------------------------------------------- */
static void test_guards(void)
{
    SUITE("bbc_video_render_row guards");

    uint8_t ram[0x8000] = {0};
    bbc_video_t vid;
    bbc_video_init(&vid, ram, sizeof(ram));
    setup_mode0(&vid);

    uint8_t buf[OUT_W];
    memset(buf, 0xAA, OUT_W);

    /* out_y < 0: should not crash, buffer untouched or zeroed */
    bbc_video_render_row(&vid, -1, OUT_H, buf, OUT_W);
    ASSERT_TRUE(1); /* just no crash */

    /* out_y >= out_height */
    bbc_video_render_row(&vid, OUT_H, OUT_H, buf, OUT_W);
    ASSERT_TRUE(1);

    /* null out_pixels: should not crash */
    bbc_video_render_row(&vid, 0, OUT_H, NULL, OUT_W);
    ASSERT_TRUE(1);

    /* out_width == 0 */
    bbc_video_render_row(&vid, 0, OUT_H, buf, 0);
    ASSERT_TRUE(1);
}

/* --------------------------------------------------------------------------
 * Test: bitmap mode — render_row matches render_frame
 * -------------------------------------------------------------------------- */
static void test_bitmap_row_matches_frame(void)
{
    SUITE("bitmap render_row == render_frame (MODE 0)");

    uint8_t *ram = (uint8_t *)calloc(0x8000, 1);
    fill_ram_pattern(ram, 0x8000);

    bbc_video_t vid;
    bbc_video_init(&vid, ram, 0x8000);
    setup_mode0(&vid);

    int mismatches = test_row_matches_frame(&vid, "MODE0");
    ASSERT_EQ(mismatches, 0);

    free(ram);
}

/* --------------------------------------------------------------------------
 * Test: all-zero RAM — render_row matches render_frame (uniform colour)
 * -------------------------------------------------------------------------- */
static void test_blank_screen(void)
{
    SUITE("blank screen: render_row matches render_frame");

    uint8_t ram[0x8000];
    memset(ram, 0, sizeof(ram));

    bbc_video_t vid;
    bbc_video_init(&vid, ram, sizeof(ram));
    setup_mode0(&vid);

    int mismatches = test_row_matches_frame(&vid, "blank");
    ASSERT_EQ(mismatches, 0);
}

/* --------------------------------------------------------------------------
 * Test: output width narrower than BBC_FB_WIDTH
 * -------------------------------------------------------------------------- */
static void test_narrow_output(void)
{
    SUITE("narrow output width (320 pixels)");

    uint8_t *ram = (uint8_t *)calloc(0x8000, 1);
    fill_ram_pattern(ram, 0x8000);

    bbc_video_t vid;
    bbc_video_init(&vid, ram, 0x8000);
    setup_mode0(&vid);

    /* render_frame at 320 wide */
    uint8_t ref_fb[320 * OUT_H];
    memset(ref_fb, 0, sizeof(ref_fb));
    bbc_video_output_t out = {
        .format = BBC_FB_FORMAT_INDEX8,
        .width = 320, .height = OUT_H,
        .framebuffer = ref_fb, .fb_stride = 320,
    };
    bbc_video_set_output(&vid, &out);
    bbc_video_render_frame(&vid);

    /* compare row by row */
    uint8_t row_buf[320];
    int mismatches = 0;
    for (int y = 0; y < OUT_H; y++) {
        memset(row_buf, 0xBB, 320);
        bbc_video_render_row(&vid, y, OUT_H, row_buf, 320);
        const uint8_t *ref = ref_fb + y * 320;
        for (int x = 0; x < 320; x++) {
            if (row_buf[x] != ref[x]) mismatches++;
        }
    }
    ASSERT_EQ(mismatches, 0);

    free(ram);
}

/* --------------------------------------------------------------------------
 * Test: teletext mode — render_row matches render_frame
 * -------------------------------------------------------------------------- */
static void test_teletext_row_matches_frame(void)
{
    SUITE("teletext render_row == render_frame (MODE 7)");

    uint8_t *ram = (uint8_t *)calloc(0x8000, 1);
    fill_ram_teletext(ram, 0x8000);

    bbc_video_t vid;
    bbc_video_init(&vid, ram, 0x8000);
    setup_mode7(&vid);

    int mismatches = test_row_matches_frame(&vid, "MODE7");
    ASSERT_EQ(mismatches, 0);

    free(ram);
}

/* --------------------------------------------------------------------------
 * Test: output pixel values are valid BBC colour indices (0-7)
 * -------------------------------------------------------------------------- */
static void test_pixel_values_valid(void)
{
    SUITE("pixel values 0-7 only");

    uint8_t *ram = (uint8_t *)calloc(0x8000, 1);
    fill_ram_pattern(ram, 0x8000);

    bbc_video_t vid;
    bbc_video_init(&vid, ram, 0x8000);
    setup_mode0(&vid);

    uint8_t buf[OUT_W];
    bool all_valid = true;
    for (int y = 0; y < OUT_H; y++) {
        bbc_video_render_row(&vid, y, OUT_H, buf, OUT_W);
        for (int x = 0; x < OUT_W; x++) {
            if (buf[x] > 7) { all_valid = false; break; }
        }
        if (!all_valid) break;
    }
    ASSERT_TRUE(all_valid);

    free(ram);
}

/* --------------------------------------------------------------------------
 * Test: different out_height values (480 for direct VGA mapping)
 * render_row(vga_line, 480) must match render_frame at out_height=480
 * -------------------------------------------------------------------------- */
static void test_vga_height_mapping(void)
{
    SUITE("out_height=480 (VGA direct mapping)");

    uint8_t *ram = (uint8_t *)calloc(0x8000, 1);
    fill_ram_pattern(ram, 0x8000);

    bbc_video_t vid;
    bbc_video_init(&vid, ram, 0x8000);
    setup_mode0(&vid);

    /* render_frame at height=480 */
    uint8_t *ref_fb = (uint8_t *)calloc(OUT_W * 480, 1);
    bbc_video_output_t out = {
        .format = BBC_FB_FORMAT_INDEX8,
        .width = OUT_W, .height = 480,
        .framebuffer = ref_fb, .fb_stride = OUT_W,
    };
    bbc_video_set_output(&vid, &out);
    bbc_video_render_frame(&vid);

    /* render_row at out_height=480 must match render_frame at out_height=480 */
    uint8_t row_buf[OUT_W];
    int mismatches = 0;
    for (int vga_line = 0; vga_line < 480; vga_line++) {
        bbc_video_render_row(&vid, vga_line, 480, row_buf, OUT_W);
        const uint8_t *ref = ref_fb + vga_line * OUT_W;
        for (int x = 0; x < OUT_W; x++) {
            if (row_buf[x] != ref[x]) mismatches++;
        }
    }
    ASSERT_EQ(mismatches, 0);

    free(ref_fb);
    free(ram);
}

/* --------------------------------------------------------------------------
 * Test: render_row does not modify video state (idempotent reads)
 * -------------------------------------------------------------------------- */
static void test_idempotent(void)
{
    SUITE("render_row is idempotent (same row twice = same output)");

    uint8_t *ram = (uint8_t *)calloc(0x8000, 1);
    fill_ram_pattern(ram, 0x8000);

    bbc_video_t vid;
    bbc_video_init(&vid, ram, 0x8000);
    setup_mode0(&vid);

    uint8_t buf1[OUT_W], buf2[OUT_W];
    int mismatches = 0;
    for (int y = 0; y < OUT_H; y += 8) {
        bbc_video_render_row(&vid, y, OUT_H, buf1, OUT_W);
        bbc_video_render_row(&vid, y, OUT_H, buf2, OUT_W);
        for (int x = 0; x < OUT_W; x++)
            if (buf1[x] != buf2[x]) mismatches++;
    }
    ASSERT_EQ(mismatches, 0);

    free(ram);
}

/* --------------------------------------------------------------------------
 * Test: render_row on teletext at VGA height=480
 * -------------------------------------------------------------------------- */
static void test_teletext_vga_height(void)
{
    SUITE("teletext render_row out_height=480");

    uint8_t *ram = (uint8_t *)calloc(0x8000, 1);
    fill_ram_teletext(ram, 0x8000);

    bbc_video_t vid;
    bbc_video_init(&vid, ram, 0x8000);
    setup_mode7(&vid);

    /* Render reference at height=480 (same out_height as render_row calls) */
    uint8_t *ref_fb = (uint8_t *)calloc(OUT_W * 480, 1);
    bbc_video_output_t out = {
        .format = BBC_FB_FORMAT_INDEX8,
        .width = OUT_W, .height = 480,
        .framebuffer = ref_fb, .fb_stride = OUT_W,
    };
    bbc_video_set_output(&vid, &out);
    saa5050_reset(&vid.teletext);
    bbc_video_render_frame(&vid);

    /* render_row at out_height=480 must match render_frame at out_height=480 */
    uint8_t row_buf[OUT_W];
    int mismatches = 0;
    for (int vga_line = 0; vga_line < 480; vga_line++) {
        saa5050_reset(&vid.teletext);
        bbc_video_render_row(&vid, vga_line, 480, row_buf, OUT_W);
        const uint8_t *ref = ref_fb + vga_line * OUT_W;
        for (int x = 0; x < OUT_W; x++)
            if (row_buf[x] != ref[x]) mismatches++;
    }
    ASSERT_EQ(mismatches, 0);

    free(ref_fb);
    free(ram);
}

/* --------------------------------------------------------------------------
 * Main
 * -------------------------------------------------------------------------- */
int main(void)
{
    printf("bbc_video tests\n");
    printf("===============\n");

    test_guards();
    test_blank_screen();
    test_bitmap_row_matches_frame();
    test_narrow_output();
    test_pixel_values_valid();
    test_vga_height_mapping();
    test_idempotent();
    test_teletext_row_matches_frame();
    test_teletext_vga_height();

    printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
