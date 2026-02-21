/*
 * saa5050.h — SAA5050 Teletext Character Generator emulation
 *
 * The SAA5050 reads BBC Micro MODE 7 RAM (1 KB at &7C00-&7FFF) and renders
 * Teletext characters (40×25) with full control-code processing:
 *   - 7 foreground colours + 7 background colours
 *   - Normal and graphic character sets (sixels)
 *   - Separated and contiguous graphics
 *   - Double height (spanning two character rows)
 *   - Flash (toggled ~1 Hz)
 *   - Hold graphics
 *
 * Character ROM: 96 characters (0x20-0x7F), 6×10 pixels each (6 bytes/row,
 * 10 rows). Stored in saa5050_font.c (960 bytes, public domain data).
 *
 * Rendering model:
 *   - Call saa5050_start_row() at the start of each character row.
 *   - For each scanline within the row, call saa5050_start_scanline() then
 *     saa5050_render_char() for each of the 40 columns left to right.
 *   - Each call produces 12 output pixels (colour index 0-7).
 *
 * References: SAA5050 datasheet, jsbeeb teletext.js (GPL-3, reference only),
 *             B-em video.c MODE 7 section (GPL-2, reference only).
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

/* Scanlines per character row (the 6845 is programmed for 10, but the
 * SAA5050 doubles each via its internal interlace → 20 visible lines/row) */
#define SAA5050_SCANLINES_PER_ROW   20

/* Output pixels per character (always 12 for the hi-res interpolated output) */
#define SAA5050_PIXELS_PER_CHAR     12

/* Character ROM dimensions */
#define SAA5050_CHAR_COUNT          96   /* chars 0x20-0x7F */
#define SAA5050_CHAR_ROWS           10   /* scanlines per char in ROM         */
#define SAA5050_CHAR_COLS           6    /* pixel columns per char in ROM     */

/* --------------------------------------------------------------------------
 * State for a single scanline pass through 40 characters.
 * Reset by saa5050_start_scanline() at the start of each scanline.
 * -------------------------------------------------------------------------- */
typedef struct {
    uint8_t  fg_colour;       /* Current foreground colour (1-7)             */
    uint8_t  bg_colour;       /* Current background colour (0-7)             */
    bool     graphics_mode;   /* true = graphics character set               */
    bool     separated_gfx;   /* true = separated (gapped) graphics          */
    bool     flash;           /* true = flashing text active                 */
    bool     hold_graphics;   /* true = hold mode active                     */
    bool     double_height;   /* true = double height for this char          */
    uint8_t  held_char;       /* Last graphic char (for hold mode)           */
    bool     held_is_gfx;     /* true if held_char came from graphics set    */
    bool     held_is_sep;     /* true if held_char came from separated set   */
} saa5050_line_state_t;

/* --------------------------------------------------------------------------
 * Full SAA5050 state
 * -------------------------------------------------------------------------- */
typedef struct {
    /* Character ROM pointer (use NULL to select the built-in ROM) */
    const uint8_t *char_rom;  /* [SAA5050_CHAR_COUNT][SAA5050_CHAR_ROWS][SAA5050_CHAR_COLS] */

    /* Per-frame state */
    bool     flash_state;     /* Current flash phase (toggle ~1 Hz)          */
    uint8_t  flash_counter;   /* Frame counter for flash timing              */

    /* Double-height row tracking (25 entries, one per character row) */
    bool     dh_row_bottom[SAA5050_ROWS]; /* true = this row is DH bottom half*/
    bool     dh_seen_this_row;            /* DH code seen during current row  */

    /* Current rendering context */
    uint8_t  current_row;     /* 0-24                                        */
    uint8_t  current_scanline;/* 0-19 within the row                        */
} saa5050_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/*
 * Initialise the SAA5050.
 * char_rom: pointer to the character ROM data (96 × 10 × 6 bytes).
 *           Pass NULL to use the built-in ROM from saa5050_font.c.
 */
void saa5050_init(saa5050_t *tt, const uint8_t *char_rom);
void saa5050_reset(saa5050_t *tt);

/*
 * Call at the start of each new character row (before rendering any chars).
 * row_num: 0-24.
 */
void saa5050_start_row(saa5050_t *tt, uint8_t row_num);

/*
 * Call at the start of each scanline (after saa5050_start_row).
 * Initialises the scanline-level state (fg=white, bg=black, no gfx, etc.)
 * so that control codes are processed fresh for each scanline.
 *
 * scanline: 0-19 within the character row.
 */
void saa5050_start_scanline(saa5050_t *tt, saa5050_line_state_t *ls,
                              uint8_t scanline);

/*
 * Render one character.
 *
 * ls:        line state (maintained across the 40-column scan)
 * char_code: 7-bit value from video RAM (0x00-0x7F)
 * out_pixels: output array of SAA5050_PIXELS_PER_CHAR entries (colour 0-7)
 *
 * Control codes (0x00-0x1F) update ls and display either a space or the
 * held graphic.  Printable chars render the appropriate glyph.
 */
void saa5050_render_char(saa5050_t *tt, saa5050_line_state_t *ls,
                          uint8_t char_code, uint8_t *out_pixels);

/*
 * Toggle flash state. Call from the 1 Hz (or 2 Hz) timer.
 * Internally counts frames and sets flash_state at the right ratio.
 */
void saa5050_toggle_flash(saa5050_t *tt);

/* Get the built-in character ROM (defined in saa5050_font.c) */
const uint8_t *saa5050_get_builtin_rom(void);

#ifdef __cplusplus
}
#endif
