/*
 * saa5050.c — SAA5050 Teletext character generator emulation
 *
 * Modelled after the Mike Stirling / David Banks VHDL for BBC Micro on FPGA.
 *
 * Key design points:
 *
 *  1. line_counter 0-9 (not 0-19).  The SAA5050 doubles each line internally.
 *     bbc_video.c calls saa5050_render_line() once per line_counter value
 *     and writes the resulting pixels twice to consecutive output scanlines.
 *
 *  2. Double-height (VHDL signals double_high1, double_high2):
 *       double_high1 is set when \x0D is encountered during a row's render.
 *       double_high2 = double_high1 from the previous row (latched at row end).
 *       line_addr:
 *         normal:          line_counter          (0-9)
 *         DH top half:     line_counter / 2      (0-4)  [double_high2 == 0]
 *         DH bot half:    (line_counter / 2) + 5 (5-9)  [double_high2 == 1]
 *       On a bottom-half row, any character without double_high1 active is
 *       replaced with a space.
 *
 *  3. Set-After: all control codes take effect starting with the NEXT
 *     character.  Implemented with _next shadow registers that are committed
 *     at the start of each character.
 *
 *  4. Hold graphics: last_gfx / last_gfx_sep store the most recent graphic
 *     character code.  During hold mode, a control-code cell shows last_gfx
 *     instead of a space.  last_gfx is cleared when entering alpha mode.
 *
 *  5. Graphics rendering: sixels decoded from the character code (same as
 *     before).  Separated graphics blank col 2, col 5, and the bottom row of
 *     each third (rows 2, 5, 9 of the 0-9 range within line_counter).
 *
 * References: Mike Stirling / David Banks VHDL SAA5050 (BBC Micro for DE1).
 * Licence: zlib
 * Copyright (c) 2026 esp-beep project
 */

#include <string.h>
#include "saa5050.h"

#ifdef ESP_PLATFORM
#  include "esp_log.h"
#  include "esp_attr.h"
#  define TT_LOGD(fmt, ...) ESP_LOGD("saa5050", fmt, ##__VA_ARGS__)
#  define RENDER_IRAM IRAM_ATTR
#else
#  define TT_LOGD(fmt, ...) /* no-op */
#  define RENDER_IRAM
#endif

/* ROM data lives in saa5050_font.c.
 * Layout: saa5050_builtin_rom[char_index][row][col]
 *   char_index = char_code - 0x20  (0 = space, 95 = DEL)
 *   row = 0-9 (line_counter / line_addr)
 *   col = 0-5 (left to right)
 * Each entry is 0 (off) or 1 (on). */
extern const uint8_t saa5050_builtin_rom[96][10][6];

/* --------------------------------------------------------------------------
 * Sixel (graphic character) row renderer
 *
 * Decodes the sixel bits from char_code and produces 6 pixel values (0/1)
 * for the given line_addr (0-9).  Separated graphics blank the separator
 * pixels.
 *
 * Bit layout (SAA5050 / UK teletext):
 *   code 0x20-0x3F: sixels in bits [5:0] of (code & 0x3F)
 *   code 0x60-0x7F: sixels in bits [5:0] of ((code - 0x20) & 0x3F)
 *     bit 0 = top-left,    bit 1 = top-right
 *     bit 2 = middle-left, bit 3 = middle-right
 *     bit 4 = bottom-left, bit 5 = bottom-right  (note: not bit 6)
 *
 * The three thirds occupy line_addr rows:
 *   top:    0-2   (rows 0, 1, 2)
 *   middle: 3-6   (rows 3, 4, 5, 6)
 *   bottom: 7-9   (rows 7, 8, 9)
 *
 * Separated graphics blank: col 2, col 5, and rows 2, 6, 9.
 * -------------------------------------------------------------------------- */
static void render_sixel_row(uint8_t char_code, uint8_t line_addr,
                              bool separated, uint8_t *out6)
{
    /* Decode sixel bits */
    uint8_t sixel;
    if (char_code >= 0x60)
        sixel = (uint8_t)((char_code - 0x20) & 0x3F);
    else
        sixel = (uint8_t)(char_code & 0x3F);

    /* Which third? */
    uint8_t left_bit, right_bit;
    if (line_addr <= 2) {
        left_bit  = (sixel >> 0) & 1;
        right_bit = (sixel >> 1) & 1;
    } else if (line_addr <= 6) {
        left_bit  = (sixel >> 2) & 1;
        right_bit = (sixel >> 3) & 1;
    } else {
        left_bit  = (sixel >> 4) & 1;
        right_bit = (sixel >> 5) & 1;
    }

    /* Fill left 3 and right 3 pixels */
    out6[0] = left_bit;
    out6[1] = left_bit;
    out6[2] = left_bit;
    out6[3] = right_bit;
    out6[4] = right_bit;
    out6[5] = right_bit;

    /* Separated graphics: blank separator columns and separator rows */
    if (separated) {
        out6[2] = 0;
        out6[5] = 0;
        if (line_addr == 2 || line_addr == 6 || line_addr == 9) {
            out6[0] = out6[1] = out6[2] = 0;
            out6[3] = out6[4] = out6[5] = 0;
        }
    }
}

/* --------------------------------------------------------------------------
 * Is this char code a graphics character (sixel)?
 *
 * In graphics mode: codes 0x20-0x3F and 0x60-0x7F are sixel graphics.
 * Codes 0x40-0x5F are alphanumeric even in graphics mode.
 * -------------------------------------------------------------------------- */
static inline bool is_gfx_char(uint8_t code, bool gfx_mode)
{
    if (!gfx_mode) return false;
    return (code >= 0x20 && code <= 0x3F) || (code >= 0x60 && code <= 0x7F);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void saa5050_init(saa5050_t *tt, const uint8_t *char_rom)
{
    memset(tt, 0, sizeof(*tt));
    tt->char_rom = char_rom ? char_rom : (const uint8_t *)saa5050_builtin_rom;
    saa5050_reset(tt);
}

void saa5050_reset(saa5050_t *tt)
{
    tt->flash_state   = false;
    tt->flash_counter = 0;
    tt->double_high1  = false;
    tt->double_high2  = false;
    tt->current_row   = 0;
}

void saa5050_reset_frame(saa5050_t *tt)
{
    tt->double_high1 = false;
    tt->double_high2 = false;
    tt->current_row  = 0;
}

RENDER_IRAM void saa5050_start_row(saa5050_t *tt, uint8_t row_num)
{
    if (row_num >= SAA5050_ROWS) return;  /* silently ignore out-of-range */
    tt->current_row  = row_num;
    tt->double_high1 = false;   /* will be set if \x0D seen during this row */
}

RENDER_IRAM void saa5050_end_row(saa5050_t *tt)
{
    /* Latch: double_high2 ← double_high1 */
    tt->double_high2 = tt->double_high1;
}

/* --------------------------------------------------------------------------
 * saa5050_render_line — render all 40 characters for line_counter lc.
 *
 * Implements VHDL-accurate Set-After control code processing and
 * double-height ROM address selection.
 * -------------------------------------------------------------------------- */
RENDER_IRAM void saa5050_render_line(saa5050_t *tt,
                                      const uint8_t *ram,
                                      uint8_t lc,
                                      uint8_t *out_pixels)
{
    /* Determine line_addr for ROM lookup (VHDL logic) */
    uint8_t line_addr;
    bool dh2 = tt->double_high2; /* latched from previous row */

    if (!dh2) {
        /* Normal or DH top half: line_addr = line_counter or line_counter/2 */
        /* We don't know yet which chars are DH until we scan for \x0D.
         * But in the VHDL, ALL characters on a DH-top row use line_counter/2.
         * The double_high1 flag on the *previous* row (double_high2) decides
         * whether this row is a bottom half.  If dh2=0 and some chars have
         * \x0D then this row is a top half for those chars. */
        line_addr = lc;  /* default: normal */
    } else {
        /* DH bottom half row */
        line_addr = (uint8_t)((lc >> 1) + 5);
        if (line_addr > 9) line_addr = 9;
    }

    /* Set-After state.  Initialise to defaults at start of each scanline. */
    uint8_t  fg          = 7;   /* white */
    uint8_t  bg          = 0;   /* black */
    bool     gfx         = false;
    bool     sep         = false;
    bool     flash_on    = false;
    bool     hold        = false;
    bool     dh_active   = false; /* double_high1 active for current char */
    uint8_t  last_gfx    = 0x20;
    bool     last_gfx_sep = false;

    /* _next mirrors (Set-After: committed at start of next char) */
    uint8_t  next_fg     = 7;
    uint8_t  next_bg     = 0;
    bool     next_gfx    = false;
    bool     next_sep    = false;
    bool     next_flash  = false;
    bool     next_hold   = false;
    bool     next_dh     = false;

    bool dh1_seen = false; /* \x0D seen during this row render */

    for (int col = 0; col < SAA5050_COLS; col++) {
        uint8_t code = ram[col] & 0x7F;

        /* --- Commit Set-After pipeline at start of this character --- */
        fg       = next_fg;
        bg       = next_bg;
        gfx      = next_gfx;
        sep      = next_sep;
        flash_on = next_flash;
        hold     = next_hold;
        dh_active = next_dh;

        bool is_ctrl = (code < 0x20);

        /* --- Process control codes (Set-After: update _next) --- */
        if (is_ctrl) {
            switch (code) {
            /* Alphanumeric colours (1-7) → text mode */
            case 1: case 2: case 3: case 4: case 5: case 6: case 7:
                next_fg  = code;
                next_gfx = false;
                last_gfx = 0x20; /* clear held char when entering alpha mode */
                break;
            case 8:  next_flash = true;  break;
            case 9:  next_flash = false; break;
            case 12: next_dh = false; break;
            case 13:
                next_dh = true;
                dh1_seen = true;
                break;
            /* Graphics colours (17-23) → graphics mode */
            case 17: case 18: case 19: case 20: case 21: case 22: case 23:
                next_fg  = (uint8_t)(code & 0x07);
                next_gfx = true;
                break;
            case 24: /* Conceal: fg = bg */
                next_fg = next_bg;
                break;
            case 25: next_sep = false; break;
            case 26: next_sep = true;  break;
            case 28: next_bg = 0;      break; /* Black background */
            case 29: next_bg = next_fg; break; /* New background = fg */
            case 30: next_hold = true;  break;
            case 31: next_hold = false; break;
            default: break;
            }
        } else {
            /* Printable char: pass _next through unchanged */
        }

        /* --- Determine ROM address and pixel data --- */

        /* On a DH bottom-half row: any char without dh_active → show space */
        uint8_t display_code = code;
        bool    this_is_gfx  = is_gfx_char(code, gfx);

        if (dh2 && !dh_active) {
            /* Bottom half but this char has no DH → show space */
            display_code = 0x20;
            this_is_gfx  = false;
        }

        /* On a DH top-half row (dh2=0, dh_active=true for this char):
         * recalculate line_addr for DH top */
        uint8_t la = line_addr; /* default */
        if (!dh2 && dh_active) {
            la = (uint8_t)(lc >> 1); /* 0-4 */
            if (la > 4) la = 4;
        }

        uint8_t raw[6] = {0,0,0,0,0,0};

        if (is_ctrl) {
            /* Control code cell: show held graphic or space */
            if (hold && last_gfx != 0x20) {
                render_sixel_row(last_gfx, la, last_gfx_sep, raw);
            }
            /* else: raw stays zero (space) */
        } else if (this_is_gfx) {
            render_sixel_row(display_code, la, sep, raw);
            /* Update last_gfx for hold mode */
            last_gfx     = display_code;
            last_gfx_sep = sep;
        } else {
            /* Text character from ROM */
            uint8_t idx = (display_code >= 0x20 && display_code < 0x80)
                          ? (uint8_t)(display_code - 0x20) : 0;
            const uint8_t *row_data =
                tt->char_rom + idx * (10 * 6) + la * 6;
            for (int i = 0; i < 6; i++)
                raw[i] = row_data[i];
        }

        /* --- Flash: blank fg during flash-off phase --- */
        uint8_t eff_fg = fg;
        if (flash_on && !tt->flash_state) {
            eff_fg = bg;
        }

        /* --- Expand 6 pixels → 12 output pixels (double horizontally) --- */
        uint8_t *dst = out_pixels + col * SAA5050_PIXELS_PER_CHAR;
        for (int i = 0; i < 6; i++) {
            uint8_t col_idx = raw[i] ? eff_fg : bg;
            dst[i * 2]     = col_idx;
            dst[i * 2 + 1] = col_idx;
        }
    }

    /* Propagate double_high1 if \x0D was seen */
    if (dh1_seen) {
        tt->double_high1 = true;
    }
}

void saa5050_toggle_flash(saa5050_t *tt)
{
    /* 48-frame on / 16-frame off = 64-frame cycle (matches jsbeeb) */
    tt->flash_counter = (uint8_t)((tt->flash_counter + 1) & 63);
    tt->flash_state   = (tt->flash_counter < 48);
}

/* --------------------------------------------------------------------------
 * Legacy per-character API — used by bbc_video_tick (MODE 7 tick path).
 *
 * This wraps the new logic to maintain compatibility with bbc_video.c's
 * tick-by-tick rendering path.  The scanline (0-19) is converted to
 * line_counter (0-9) and the Set-After pipeline is maintained in ls.
 * -------------------------------------------------------------------------- */

RENDER_IRAM void saa5050_start_scanline(saa5050_t *tt, saa5050_line_state_t *ls,
                                          uint8_t scanline)
{
    /* Store raw scanline (0-19) for line_addr calculation in render_char */
    ls->line_counter = scanline;

    ls->fg_colour    = 7;
    ls->bg_colour    = 0;
    ls->graphics_mode  = false;
    ls->separated_gfx  = false;
    ls->flash          = false;
    ls->hold_graphics  = false;
    ls->double_height  = tt->double_high2;
    ls->held_char      = 0x20;
    ls->held_is_gfx    = false;
    ls->held_is_sep    = false;
    /* Set-After next defaults */
    ls->next_fg           = 7;
    ls->next_bg           = 0;
    ls->next_graphics     = false;
    ls->next_separated    = false;
    ls->next_flash        = false;
    ls->next_hold         = false;
    ls->next_double_height = tt->double_high2;
}

RENDER_IRAM void saa5050_render_char(saa5050_t *tt, saa5050_line_state_t *ls,
                                      uint8_t char_code, uint8_t *out_pixels)
{
    char_code &= 0x7F;

    /* Commit Set-After pipeline */
    ls->fg_colour     = ls->next_fg;
    ls->bg_colour     = ls->next_bg;
    ls->graphics_mode = ls->next_graphics;
    ls->separated_gfx = ls->next_separated;
    ls->flash         = ls->next_flash;
    ls->hold_graphics = ls->next_hold;
    ls->double_height = ls->next_double_height;

    /* Use scanline stored by saa5050_start_scanline (0-19) */
    uint8_t sc = ls->line_counter;  /* raw scanline 0-19 */

    bool dh2 = tt->double_high2;
    uint8_t la;
    if (dh2) {
        /* DH bottom half: line_addr = (scanline/2)+5, clamped to 0-9 */
        la = (uint8_t)((sc >> 1) + 5);
        if (la > 9) la = 9;
    } else {
        /* Normal: line_addr = scanline/2 */
        la = (uint8_t)(sc >> 1);
        if (la > 9) la = 9;
    }

    bool is_ctrl = (char_code < 0x20);

    if (is_ctrl) {
        switch (char_code) {
        case 1: case 2: case 3: case 4: case 5: case 6: case 7:
            ls->next_fg       = char_code;
            ls->next_graphics = false;
            ls->held_char     = 0x20;
            break;
        case 8:  ls->next_flash    = true;  break;
        case 9:  ls->next_flash    = false; break;
        case 12: ls->next_double_height = false; break;
        case 13:
            ls->next_double_height = true;
            tt->double_high1 = true;
            break;
        case 17: case 18: case 19: case 20: case 21: case 22: case 23:
            ls->next_fg       = (uint8_t)(char_code & 0x07);
            ls->next_graphics = true;
            break;
        case 24: ls->next_fg = ls->next_bg; break;
        case 25: ls->next_separated = false; break;
        case 26: ls->next_separated = true;  break;
        case 28: ls->next_bg = 0;            break;
        case 29: ls->next_bg = ls->next_fg;  break;
        case 30: ls->next_hold = true;       break;
        case 31: ls->next_hold = false;      break;
        default: break;
        }
    }

    uint8_t raw[6] = {0,0,0,0,0,0};
    bool this_is_gfx = is_gfx_char(char_code, ls->graphics_mode);

    if (is_ctrl) {
        if (ls->hold_graphics && ls->held_char != 0x20) {
            render_sixel_row(ls->held_char, la, ls->held_is_sep, raw);
        }
    } else if (this_is_gfx) {
        render_sixel_row(char_code, la, ls->separated_gfx, raw);
        ls->held_char   = char_code;
        ls->held_is_gfx = true;
        ls->held_is_sep = ls->separated_gfx;
    } else {
        uint8_t idx = (char_code >= 0x20 && char_code < 0x80)
                      ? (uint8_t)(char_code - 0x20) : 0;
        const uint8_t *row_data = tt->char_rom + idx * (10 * 6) + la * 6;
        for (int i = 0; i < 6; i++) raw[i] = row_data[i];
        if (ls->graphics_mode) { ls->held_char = 0x20; ls->held_is_gfx = false; }
    }

    uint8_t eff_fg = ls->fg_colour;
    if (ls->flash && !tt->flash_state) eff_fg = ls->bg_colour;

    for (int i = 0; i < 6; i++) {
        uint8_t c = raw[i] ? eff_fg : ls->bg_colour;
        out_pixels[i * 2]     = c;
        out_pixels[i * 2 + 1] = c;
    }
}
