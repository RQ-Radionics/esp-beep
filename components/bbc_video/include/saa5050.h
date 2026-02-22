/*
 * saa5050.h — SAA5050 Teletext Character Generator emulation
 *
 * Modelled after the Mike Stirling / David Banks VHDL implementation for
 * the BBC Micro on FPGA (BBC Micro for DE1).  The key differences from
 * earlier versions:
 *
 *   • line_counter 0-9 per character row (not 0-19 scanlines).
 *     The SAA5050 produces each glyph row twice internally (interlace doubling)
 *     so the caller only iterates line_counter 0-9 per character row.
 *
 *   • Double-height uses double_high1 / double_high2 (two flags, mirroring
 *     the VHDL signal names):
 *       double_high1  — \x0D was seen in the current row (Set-After: applies
 *                       to all chars after the code in the SAME row and the
 *                       following row).
 *       double_high2  — latched at row end: double_high2 ← double_high1.
 *       line_addr for DH top  (double_high2 = 0): line_counter / 2   → 0-4
 *       line_addr for DH bot  (double_high2 = 1): line_counter/2 + 5 → 5-9
 *       line_addr for normal:                     line_counter        → 0-9
 *
 *   • All control codes are Set-After (effect starts on the character
 *     immediately after the control code, not the control code itself).
 *     This is modelled with a one-character pipeline (_next signals).
 *
 *   • Hold graphics: last_gfx / last_gfx_sep track the most recent graphic
 *     character; during hold mode a control-code cell shows the held glyph.
 *
 * Rendering model (per frame):
 *   saa5050_reset_frame()        — at frame start
 *   for each row 0-24:
 *     saa5050_start_row()        — at row start (propagates DH state)
 *     for each line_counter 0-9:
 *       saa5050_render_line()    — renders all 40 chars for this line_counter
 *                                  into a 480-pixel-wide output buffer
 *     saa5050_end_row()          — at row end (latches DH state)
 *
 * References: Mike Stirling / David Banks VHDL SAA5050 for BBC Micro on FPGA.
 * Licence: zlib
 * Copyright (c) 2026 esp-beep project
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Number of rows/cols in MODE 7 */
#define SAA5050_COLS    40
#define SAA5050_ROWS    25

/* line_counter values per character row (VHDL: 0 to 9) */
#define SAA5050_LINES_PER_ROW   10

/* Output pixels per character (6 ROM pixels doubled horizontally → 12) */
#define SAA5050_PIXELS_PER_CHAR 12

/* Scanlines emitted per character row: each line_counter value is rendered
 * twice (SAA5050 internal doubling), so 10 × 2 = 20 scanlines per row. */
#define SAA5050_SCANLINES_PER_ROW   20

/* --------------------------------------------------------------------------
 * Full SAA5050 state
 * -------------------------------------------------------------------------- */
typedef struct {
    /* Character ROM pointer (NULL → use built-in ROM from saa5050_font.c) */
    const uint8_t *char_rom; /* [96][10] — text glyphs only (chars 0x20-0x7F) */

    /* Flash */
    bool     flash_state;
    uint8_t  flash_counter;

    /* Double-height state (VHDL names) */
    bool     double_high1;   /* \x0D seen in current row (Set-After)  */
    bool     double_high2;   /* latched at end of previous row        */

    /* Current row (0-24) */
    uint8_t  current_row;
} saa5050_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void saa5050_init(saa5050_t *tt, const uint8_t *char_rom);
void saa5050_reset(saa5050_t *tt);

/* Call at start of each frame (resets double-height state). */
void saa5050_reset_frame(saa5050_t *tt);

/* Call at the start of each character row (row 0-24). */
void saa5050_start_row(saa5050_t *tt, uint8_t row_num);

/* Call at the end of each character row (latches double_high2). */
void saa5050_end_row(saa5050_t *tt);

/*
 * Render one complete character row for line_counter value lc (0-9).
 *
 * ram[40]: the 40 bytes of video RAM for this row (values 0x00-0x7F).
 * out_pixels[40 * SAA5050_PIXELS_PER_CHAR]: output colour indices (0-7).
 *
 * Internally this handles all control code processing (Set-After pipeline),
 * hold graphics, double-height ROM address selection, and flash.
 */
void saa5050_render_line(saa5050_t *tt,
                          const uint8_t *ram,   /* 40 bytes */
                          uint8_t        lc,    /* line_counter 0-9 */
                          uint8_t       *out_pixels); /* 480 bytes */

/* Toggle flash (~1 Hz). Call once per frame. */
void saa5050_toggle_flash(saa5050_t *tt);

/* --- Legacy per-character API (kept for bbc_video_tick compatibility) --- */

typedef struct {
    uint8_t  fg_colour;
    uint8_t  bg_colour;
    bool     graphics_mode;
    bool     separated_gfx;
    bool     flash;
    bool     hold_graphics;
    bool     double_height;
    uint8_t  held_char;
    bool     held_is_gfx;
    bool     held_is_sep;
    /* Set-After pipeline */
    uint8_t  next_fg;
    uint8_t  next_bg;
    bool     next_graphics;
    bool     next_separated;
    bool     next_flash;
    bool     next_hold;
    bool     next_double_height;
    /* Scanline tracking for line_addr calculation */
    uint8_t  line_counter;   /* raw scanline 0-19 from saa5050_start_scanline */
} saa5050_line_state_t;

void saa5050_start_scanline(saa5050_t *tt, saa5050_line_state_t *ls,
                              uint8_t scanline);
void saa5050_render_char(saa5050_t *tt, saa5050_line_state_t *ls,
                          uint8_t char_code, uint8_t *out_pixels);

/* Get built-in ROM */
const uint8_t *saa5050_get_builtin_rom(void);

#ifdef __cplusplus
}
#endif
