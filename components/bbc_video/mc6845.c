/*
 * mc6845.c — MC6845 CRTC emulation for BBC Micro / ESP32-ESP-IDF
 *
 * Derived from floooh/chips mc6845.h by Andre Weissflog (zlib licence).
 * Rewritten to follow the VHDL reference implementation closely:
 *   - Horizontal counter checks wrap BEFORE incrementing (VHDL style).
 *   - Vertical advance is inline in the h_total branch.
 *   - VSYNC counter increments once per scanline (at h_ctr == h_sync_pos).
 *   - Cursor uses a cursor_line_ff that persists across scanlines.
 *   - in_vadj is a struct field (VHDL process variable persisting across clocks).
 *
 * Licence: zlib
 * Copyright (c) 2018 Andre Weissflog
 * Adaptation (c) 2026 esp-beep project
 */

#include <string.h>
#include "mc6845.h"

#ifdef ESP_PLATFORM
#  include "esp_log.h"
#  define CRTC_LOGD(fmt, ...) ESP_LOGD("mc6845", fmt, ##__VA_ARGS__)
#else
#  define CRTC_LOGD(fmt, ...) /* no-op */
#endif

/* --------------------------------------------------------------------------
 * Register access masks — how many bits are valid per register
 * -------------------------------------------------------------------------- */
static const uint8_t s_mask[18] = {
    0xFF, /* R0  HTOTAL          */
    0xFF, /* R1  HDISPLAYED      */
    0xFF, /* R2  HSYNCPOS        */
    0xFF, /* R3  SYNCWIDTHS      */
    0x7F, /* R4  VTOTAL          */
    0x1F, /* R5  VTOTALADJ       */
    0x7F, /* R6  VDISPLAYED      */
    0x7F, /* R7  VSYNCPOS        */
    0xF3, /* R8  INTERLACEMODE   */
    0x1F, /* R9  MAXSCANLINEADDR */
    0x7F, /* R10 CURSORSTART     */
    0x1F, /* R11 CURSOREND       */
    0x3F, /* R12 STARTADDRHI     */
    0xFF, /* R13 STARTADDRLO     */
    0x3F, /* R14 CURSORHI        */
    0xFF, /* R15 CURSORLO        */
    0x3F, /* R16 LIGHTPENHI      */
    0xFF, /* R17 LIGHTPENLO      */
};

/* Readable/writable flags per chip type and register.
   Bit 0: writable, bit 1: readable. */
static const uint8_t s_rw[3][18] = {
    /* UM6845 (type 0) */
    { 1,1,1,1,1,1,1,1,1,1,1,1,3,3,3,3,2,2 },
    /* UM6845R (type 1) */
    { 1,1,1,1,1,1,1,1,1,1,1,1,1,1,3,3,2,2 },
    /* MC6845 (type 2) — BBC Micro */
    { 1,1,1,1,1,1,1,1,1,1,1,1,1,1,3,3,2,2 },
};

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

static inline uint8_t _hsync_width(const mc6845_t *c)
{
    uint8_t w = c->sync_widths & 0x0F;
    if (c->type == MC6845_TYPE_MC6845 && w == 0)
        w = 16;
    return w;
}

static inline uint8_t _vsync_width(const mc6845_t *c)
{
    uint8_t w;
    if (c->type == MC6845_TYPE_UM6845) {
        w = (c->sync_widths >> 4) & 0x0F;
        if (w == 0) w = 16;
    } else {
        w = 16; /* fixed on MC6845 and UM6845R */
    }
    return w;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void mc6845_init(mc6845_t *c, mc6845_type_t type)
{
    memset(c, 0, sizeof(*c));
    c->type = type;
    /* UM6845R register 31 reads as 0xFF */
    c->reg[0x1F] = 0xFF;
    mc6845_reset(c);
}

void mc6845_reset(mc6845_t *c)
{
    c->h_ctr      = 0;
    c->hsync_ctr  = 0;
    c->v_ctr      = 0;
    c->r_ctr      = 0;
    c->vsync_ctr  = 0;
    c->vadj_ctr   = 0;
    c->ma         = 0;
    c->ma_row_start = 0;
    c->ma_store   = 0;
    c->hs         = false;
    c->vs         = false;
    c->h_de       = false;
    c->v_de       = false;
    c->in_vadj    = false;
    c->odd_field  = false;
    c->cursor_on  = false;
    c->cursor_line_ff     = false;
    c->cursor_blink_ctr   = 0;
    c->cursor_blink_state = true;
    c->frame_count = 0;
    c->lightpen_latched = false;
    CRTC_LOGD("reset");
}

void mc6845_write(mc6845_t *c, uint8_t addr, uint8_t data)
{
    if (!(addr & 1)) {
        /* Write to address register */
        c->sel = data & 0x1F;
    } else {
        /* Write to selected data register */
        int i = c->sel & 0x1F;
        if (i < 18 && (s_rw[c->type][i] & 1)) {
            c->reg[i] = data & s_mask[i];
            CRTC_LOGD("R%d = %02X", i, c->reg[i]);
        }
    }
}

uint8_t mc6845_read(mc6845_t *c, uint8_t addr)
{
    if (!(addr & 1)) {
        /* Read address register — only UM6845/UM6845R return status */
        if (c->type != MC6845_TYPE_MC6845) {
            return c->v_de ? 0 : (1 << 5); /* bit 5: in vertical blank */
        }
        return 0;
    } else {
        /* Read data register */
        int i = c->sel & 0x1F;
        if (i < 18 && (s_rw[c->type][i] & 2)) {
            return c->reg[i] & s_mask[i];
        }
        return 0;
    }
}

mc6845_output_t mc6845_tick(mc6845_t *c)
{
    /* ---- HORIZONTAL COUNTER (VHDL process 1) ----
     * Check wrap BEFORE incrementing (VHDL: if h_counter = r00_h_total) */
    if (c->h_ctr == c->h_total) {
        /* ---- End of horizontal line: VERTICAL ADVANCE ---- */
        c->h_ctr = 0;

        /* Compute max_scan for this row */
        uint8_t max_scan;
        if (!c->in_vadj) {
            max_scan = c->max_scanline_addr;
        } else {
            max_scan = (c->v_total_adjust > 0) ? (c->v_total_adjust - 1) : 0;
        }

        /* need_adj: must go through vertical-adjust phase */
        bool need_adj = (c->v_total_adjust != 0) || c->odd_field;

        /* frame_end: last scanline of the last row (or last vadj scanline) */
        bool frame_end = (c->r_ctr == max_scan) &&
                         (c->in_vadj || (!need_adj && c->v_ctr == c->v_total) ||
                          (need_adj && c->v_ctr == c->v_total && c->in_vadj));

        /* Simplify: frame ends when r_ctr==max_scan AND
         *   (we're in vadj, OR v_ctr==v_total and no vadj needed) */
        frame_end = (c->r_ctr == max_scan) &&
                    (c->in_vadj ||
                     (!need_adj && c->v_ctr == c->v_total));

        if (frame_end) {
            /* End of frame */
            c->r_ctr        = 0;
            c->v_ctr        = 0;
            c->in_vadj      = false;
            c->v_de         = true;
            c->ma_row_start = mc6845_get_start_addr(c);
            c->frame_count++;
            /* Cursor blink: toggle every 8 or 16 frames */
            c->cursor_blink_ctr++;
            uint8_t blink_mode = (c->cursor_start >> 5) & 0x03;
            uint8_t blink_rate = (blink_mode == 3) ? 16 : 8;
            if (c->cursor_blink_ctr >= blink_rate) {
                c->cursor_blink_ctr   = 0;
                c->cursor_blink_state = !c->cursor_blink_state;
            }
        } else if (!c->in_vadj && c->r_ctr == max_scan) {
            /* End of character row (raster complete, not yet at v_total or in vadj) */
            c->r_ctr = 0;
            c->ma_row_start = (uint16_t)((c->ma_row_start + c->h_displayed) & 0x3FFF);
            c->v_ctr++;

            /* v_de: turns off when v_ctr reaches v_displayed */
            if (c->v_ctr == c->v_displayed) {
                c->v_de = false;
            }

            /* Enter vertical-adjust phase if needed */
            if (c->v_ctr == c->v_total && need_adj) {
                c->in_vadj = true;
            }
        } else {
            /* Next scanline within row */
            c->r_ctr++;
        }

        /* MA reloads to ma_row_start at start of each scanline */
        c->ma = c->ma_row_start;

        /* ---- DISPLAY ENABLE (horizontal) resets at line start ---- */
        c->h_de = true;

    } else {
        /* Normal horizontal advance */
        c->h_ctr++;
        c->ma = (c->ma + 1) & 0x3FFF;
    }

    /* ---- DISPLAY ENABLE (combinatorial) ---- */
    c->h_de = (c->h_ctr < c->h_displayed);
    c->v_de = (c->v_ctr < c->v_displayed);

    /* ---- HSYNC (VHDL process 2) ----
     * HSYNC starts when h_ctr == h_sync_pos and stays active for hsync_width
     * ticks.  Counter starts at 1 on the rising-edge tick.
     * Fall condition: hsync_ctr reaches hsync_width on a SUBSEQUENT tick. */
    if (c->hs) {
        /* Already active: check for fall first, then count */
        if (c->hsync_ctr >= _hsync_width(c)) {
            c->hs        = false;
            c->hsync_ctr = 0;
        } else {
            c->hsync_ctr++;
        }
    } else if (c->h_ctr == c->h_sync_pos) {
        /* Rising edge */
        c->hs        = true;
        c->hsync_ctr = 1;
    }

    /* ---- VSYNC (VHDL: evaluated when h_counter == h_sync_pos) ----
     * Rises when v_ctr == v_sync_pos and r_ctr == 0.
     * Counter increments once per scanline starting the tick AFTER the rise
     * (each subsequent time h_ctr == h_sync_pos while vs is active).
     * Falls when vsync_ctr reaches vsync_width. */
    if (c->h_ctr == c->h_sync_pos) {
        if (!c->vs && c->v_ctr == c->v_sync_pos && c->r_ctr == 0) {
            /* Rising edge — counter starts at 0 (first increment next scanline) */
            c->vs        = true;
            c->vsync_ctr = 0;
            if (c->vsync_cb)
                c->vsync_cb(c->vsync_ctx, true);
        } else if (c->vs) {
            /* Already active: count scanlines elapsed since rise */
            c->vsync_ctr++;
            if (c->vsync_ctr == _vsync_width(c)) {
                c->vs = false;
                if (c->vsync_cb)
                    c->vsync_cb(c->vsync_ctx, false);
            }
        }
    }

    /* ---- CURSOR (VHDL cursor process with cursor_line_ff) ----
     * cursor_line_ff is set when r_ctr reaches cursor_start and cleared when
     * it passes cursor_end.  It resets at start of each character (r_ctr==0). */
    uint16_t cur_addr = mc6845_get_cursor_addr(c);

    if (c->h_de && c->v_de && c->ma == cur_addr) {
        if (c->r_ctr == 0)
            c->cursor_line_ff = false;
        if (c->r_ctr == (c->cursor_start & 0x1F))
            c->cursor_line_ff = true;

        uint8_t blink_mode = (c->cursor_start >> 5) & 0x03;
        bool blink_ok;
        switch (blink_mode) {
            case 0:  blink_ok = true;  break;       /* always on            */
            case 1:  blink_ok = false; break;       /* always off           */
            default: blink_ok = c->cursor_blink_state; break; /* blink      */
        }
        c->cursor_on = c->cursor_line_ff && blink_ok;

        if (c->r_ctr == (c->cursor_end & 0x1F))
            c->cursor_line_ff = false;
    } else {
        c->cursor_on = false;
    }

    /* ---- OUTPUT ---- */
    mc6845_output_t out;
    out.ma             = c->ma & 0x3FFF;
    out.ra             = c->r_ctr & 0x1F;
    out.display_enable = c->h_de && c->v_de;
    out.hsync          = c->hs;
    out.vsync          = c->vs;
    out.cursor         = c->cursor_on;
    return out;
}

void mc6845_light_pen_strobe(mc6845_t *c)
{
    c->lightpen_addr    = c->ma;
    c->lightpen_latched = true;
    c->reg[MC6845_R16_LIGHTPENHI] = (c->ma >> 8) & 0x3F;
    c->reg[MC6845_R17_LIGHTPENLO] = c->ma & 0xFF;
}

void mc6845_set_vsync_callback(mc6845_t *c,
                                void (*cb)(void *ctx, bool state),
                                void *ctx)
{
    c->vsync_cb  = cb;
    c->vsync_ctx = ctx;
}
