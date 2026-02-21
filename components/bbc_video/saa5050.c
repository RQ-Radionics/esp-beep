/*
 * saa5050.c — SAA5050 Teletext character generator emulation
 *
 * Renders BBC Micro MODE 7 Teletext characters (40×25 rows, 20 scanlines
 * per row). Each character produces 12 output pixels (colour index 0-7).
 *
 * Control code processing is stateful per scanline: the line_state_t struct
 * must be initialised with saa5050_start_scanline() at the start of each
 * horizontal scan (left to right), and passed through saa5050_render_char()
 * for each of the 40 columns.
 *
 * References: SAA5050 datasheet, jsbeeb teletext.js (reference only, GPL-3),
 *             B-em video.c MODE 7 section (reference only, GPL-2).
 * Licence: zlib
 * Copyright (c) 2026 esp-beep project
 */

#include <string.h>
#include "saa5050.h"

#ifdef ESP_PLATFORM
#  include "esp_log.h"
#  define TT_LOGD(fmt, ...) ESP_LOGD("saa5050", fmt, ##__VA_ARGS__)
#else
#  define TT_LOGD(fmt, ...) /* no-op */
#endif

/* ROM data lives in saa5050_font.c */
extern const uint8_t saa5050_builtin_rom[96][10][6];

/* --------------------------------------------------------------------------
 * Sixel graphic character rendering
 *
 * Sixel characters occupy a 2×3 block grid within the 6×10 character cell:
 *
 *   Bit mapping (from SAA5050 datasheet):
 *     bit 0 → top-left  block  (cols 0-2, rows 0-2)
 *     bit 1 → top-right block  (cols 3-5, rows 0-2)
 *     bit 2 → mid-left  block  (cols 0-2, rows 3-6)
 *     bit 3 → mid-right block  (cols 3-5, rows 3-6)
 *     bit 4 → bot-left  block  (cols 0-2, rows 7-9)
 *     bit 5 → reserved (always 1 in graphic chars in range 0x60-0x7F)
 *     bit 6 → bot-right block  (cols 3-5, rows 7-9)
 *
 * The sixel bits come from: code & 0x3F when 0x20-0x3F,
 *                           (code - 0x40) & 0x3F when 0x60-0x7F
 *
 * Separated graphics: clear the rightmost column and bottom row of each block.
 * -------------------------------------------------------------------------- */
static void render_sixel_row(const saa5050_line_state_t *ls,
                              uint8_t char_code, uint8_t row_in_10,
                              uint8_t *out_6pixels)
{
    /* Decode sixel bits */
    uint8_t sixel;
    if (char_code >= 0x60)
        sixel = (char_code - 0x40) & 0x7F;
    else
        sixel = char_code & 0x7F;

    /* Which third of the character are we on? */
    int third;          /* 0=top, 1=mid, 2=bot */
    if      (row_in_10 <= 2) third = 0;
    else if (row_in_10 <= 6) third = 1;
    else                     third = 2;

    /* Bit indices for left and right halves */
    uint8_t left_bit, right_bit;
    switch (third) {
        case 0: left_bit = (sixel >> 0) & 1; right_bit = (sixel >> 1) & 1; break;
        case 1: left_bit = (sixel >> 2) & 1; right_bit = (sixel >> 3) & 1; break;
        default:left_bit = (sixel >> 4) & 1; right_bit = (sixel >> 6) & 1; break;
    }

    /* Separated graphics: blank edge pixels */
    uint8_t lb = left_bit, rb = right_bit;
    if (ls->separated_gfx) {
        /* Blank the last column of each half and the bottom row of each third */
        if (row_in_10 == 2 || row_in_10 == 6 || row_in_10 == 9) {
            lb = 0; rb = 0;
        }
    }

    /* Fill 6 pixels: [0..2]=left block, [3..5]=right block */
    for (int i = 0; i < 3; i++) {
        out_6pixels[i] = lb;
    }
    for (int i = 3; i < 6; i++) {
        out_6pixels[i] = rb;
    }

    /* Separated: gap between left and right (col 2 is the boundary) */
    if (ls->separated_gfx) {
        out_6pixels[2] = 0;
        out_6pixels[5] = 0;
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void saa5050_init(saa5050_t *tt, const uint8_t *char_rom)
{
    memset(tt, 0, sizeof(*tt));
    tt->char_rom     = char_rom ? char_rom : (const uint8_t *)saa5050_builtin_rom;
    tt->flash_state  = false;
    tt->flash_counter = 0;
    saa5050_reset(tt);
}

void saa5050_reset(saa5050_t *tt)
{
    tt->flash_state  = false;
    tt->flash_counter = 0;
    tt->current_row  = 0;
    tt->current_scanline = 0;
    memset(tt->dh_row_bottom, 0, sizeof(tt->dh_row_bottom));
    tt->dh_seen_this_row = false;
    TT_LOGD("reset");
}

void saa5050_start_row(saa5050_t *tt, uint8_t row_num)
{
    if (row_num >= SAA5050_ROWS) return;
    tt->current_row       = row_num;
    tt->dh_seen_this_row  = false;
    /* dh_row_bottom is set during the previous row's render */
}

void saa5050_start_scanline(saa5050_t *tt, saa5050_line_state_t *ls,
                              uint8_t scanline)
{
    tt->current_scanline = scanline;

    /* Reset per-scanline state (always starts fresh each scan) */
    ls->fg_colour     = 7;     /* white */
    ls->bg_colour     = 0;     /* black */
    ls->graphics_mode = false;
    ls->separated_gfx = false;
    ls->flash         = false;
    ls->hold_graphics = false;
    ls->double_height = tt->dh_row_bottom[tt->current_row];
    ls->held_char     = 0x20;
    ls->held_is_gfx   = false;
    ls->held_is_sep   = false;
}

void saa5050_render_char(saa5050_t *tt, saa5050_line_state_t *ls,
                          uint8_t char_code, uint8_t *out_pixels)
{
    char_code &= 0x7F;

    /* Which ROM scanline to use for this character */
    uint8_t scanline = tt->current_scanline;

    /* double height: map 20-scanline row to 10-scanline glyph half */
    uint8_t rom_row;
    if (ls->double_height) {
        /* Bottom half: scanlines 0-9 map to glyph rows 5-9 */
        rom_row = (scanline >> 1) + 5;
        if (rom_row > 9) rom_row = 9;
    } else {
        rom_row = scanline >> 1;   /* 20 scanlines → 10 ROM rows */
        if (rom_row > 9) rom_row = 9;
    }

    /* Raw pixel data for this character (6 pixels) */
    uint8_t raw[6] = {0,0,0,0,0,0};

    /* ----- Process control codes (0x00-0x1F) ----- */
    if (char_code < 0x20) {
        bool was_gfx  = ls->graphics_mode;
        bool hold_old = ls->hold_graphics;

        switch (char_code) {
        case 1: case 2: case 3: case 4: case 5: case 6: case 7:
            /* Alphanumeric colour: switch to text mode */
            ls->fg_colour     = char_code;
            ls->graphics_mode = false;
            /* Clear held char when entering text mode */
            ls->held_char    = 0x20;
            break;
        case 8:  /* Flash on */
            ls->flash = true;  break;
        case 9:  /* Steady (flash off) */
            ls->flash = false; break;
        case 12: /* Normal height */
            ls->double_height = false; break;
        case 13: /* Double height */
            ls->double_height = true;
            tt->dh_seen_this_row = true;
            break;
        case 17: case 18: case 19: case 20: case 21: case 22: case 23:
            /* Graphics colour */
            ls->fg_colour     = char_code & 0x07;
            ls->graphics_mode = true;
            break;
        case 24: /* Conceal */
            ls->fg_colour = ls->bg_colour; break;
        case 25: /* Contiguous graphics */
            ls->separated_gfx = false; break;
        case 26: /* Separated graphics */
            ls->separated_gfx = true; break;
        case 28: /* Black background */
            ls->bg_colour = 0; break;
        case 29: /* New background = current fg */
            ls->bg_colour = ls->fg_colour; break;
        case 30: /* Hold graphics */
            ls->hold_graphics = true; break;
        case 31: /* Release graphics */
            ls->hold_graphics = false; break;
        default:
            break;
        }

        /* Hold graphics: display held char instead of space */
        uint8_t display_char = 0x20; /* default: space */
        bool use_gfx = false;
        if (ls->hold_graphics || hold_old) {
            if (was_gfx && ls->held_char != 0x20) {
                display_char = ls->held_char;
                use_gfx      = ls->held_is_gfx;
            }
        }

        if (use_gfx) {
            render_sixel_row(ls, display_char, rom_row, raw);
        } else {
            /* Space glyph */
            memset(raw, 0, 6);
        }

    } else {
        /* ----- Printable characters (0x20-0x7F) ----- */
        bool is_gfx_char = ls->graphics_mode &&
                           ((char_code & 0x20) != 0 ||
                            (char_code >= 0x60 && char_code < 0x80));

        if (is_gfx_char) {
            render_sixel_row(ls, char_code, rom_row, raw);
            /* Update held char for hold mode */
            ls->held_char   = char_code;
            ls->held_is_gfx = true;
            ls->held_is_sep = ls->separated_gfx;
        } else {
            /* Text glyph from ROM */
            uint8_t idx = (char_code >= 0x20 && char_code < 0x80)
                          ? (char_code - 0x20) : 0;
            const uint8_t *row_data =
                tt->char_rom + idx * (SAA5050_CHAR_ROWS * SAA5050_CHAR_COLS)
                + rom_row * SAA5050_CHAR_COLS;
            for (int i = 0; i < SAA5050_CHAR_COLS; i++)
                raw[i] = row_data[i];
            if (ls->graphics_mode) {
                ls->held_char   = 0x20;  /* text chars don't hold */
                ls->held_is_gfx = false;
            }
        }
    }

    /* ----- Apply flash: blank fg if flash active and flash_state false ----- */
    uint8_t fg = ls->fg_colour;
    uint8_t bg = ls->bg_colour;
    if (ls->flash && !tt->flash_state) {
        fg = bg;  /* show background colour during flash-off phase */
    }

    /* ----- Expand 6 raw pixels → 12 output pixels (double horizontally) ----- */
    for (int i = 0; i < SAA5050_CHAR_COLS; i++) {
        uint8_t col = raw[i] ? fg : bg;
        out_pixels[i * 2]     = col;
        out_pixels[i * 2 + 1] = col;
    }

    /* ----- Track double-height for next row ----- */
    if (tt->current_scanline == SAA5050_SCANLINES_PER_ROW - 1) {
        /* End of row: record if DH was used */
        uint8_t next = tt->current_row + 1;
        if (next < SAA5050_ROWS) {
            tt->dh_row_bottom[next] = tt->dh_seen_this_row;
        }
    }
}

void saa5050_toggle_flash(saa5050_t *tt)
{
    /* 3:1 flash ratio: 3 frames on, 1 frame off (period = 4 frames ≈ 1.25 Hz
     * at 50 fps). jsbeeb uses 48 frames on / 16 off (64-frame cycle). */
    tt->flash_counter = (tt->flash_counter + 1) & 63;
    tt->flash_state   = (tt->flash_counter < 48);
    TT_LOGD("flash=%d counter=%d", tt->flash_state, tt->flash_counter);
}
