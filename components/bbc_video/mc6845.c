/*
 * mc6845.c — MC6845 CRTC emulation for BBC Micro / ESP32-ESP-IDF
 *
 * Derived from floooh/chips mc6845.h by Andre Weissflog (zlib licence).
 * Rewritten to use a direct register API instead of the 64-bit pin bus,
 * with cursor blink, VSYNC callback, and BBC-specific vertical-adjust.
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

/* Update all coincidence flags that depend on h_ctr */
static inline void _co_hctr(mc6845_t *c)
{
    c->co_htotal = (c->h_ctr >= (uint8_t)(c->h_total + 1));
    c->co_hdisp  = (c->h_ctr == c->h_displayed);
    c->co_hspos  = (c->h_ctr == c->h_sync_pos);
}

/* Update all coincidence flags that depend on v_ctr */
static inline void _co_vctr(mc6845_t *c)
{
    c->co_vtotal = (c->v_ctr >= (uint8_t)(c->v_total + 1));
    c->co_vdisp  = (c->v_ctr == c->v_displayed);
    c->co_vspos  = (c->v_ctr == c->v_sync_pos);
}

/* Update raster coincidence */
static inline void _co_raster(mc6845_t *c)
{
    uint8_t max = c->max_scanline_addr;
    if (c->v_ctr == c->v_total)
        max += c->v_total_adjust;
    c->co_raster = (c->r_ctr >= (uint8_t)(max + 1));
}

/* --------------------------------------------------------------------------
 * Vertical advance — called at end of every horizontal line
 * -------------------------------------------------------------------------- */
static void _advance_vertical(mc6845_t *c)
{
    if (c->in_vadj) {
        /* Vertical-adjust phase: count extra scanlines */
        c->vadj_ctr++;
        if (c->vadj_ctr >= c->v_total_adjust) {
            /* End of frame */
            c->in_vadj   = false;
            c->vadj_ctr  = 0;
            c->v_ctr     = 0;
            c->r_ctr     = 0;
            c->v_de      = true;
            c->ma_store  = mc6845_get_start_addr(c);
            c->ma_row_start = c->ma_store;
            c->ma        = c->ma_row_start;
            c->frame_count++;
            /* Cursor blink: toggle every 8 or 16 frames */
            c->cursor_blink_ctr++;
            uint8_t blink_mode = (c->cursor_start >> 5) & 0x03;
            uint8_t blink_rate = (blink_mode == 3) ? 16 : 8;
            if (c->cursor_blink_ctr >= blink_rate) {
                c->cursor_blink_ctr  = 0;
                c->cursor_blink_state = !c->cursor_blink_state;
            }
        }
        return;
    }

    /* Advance raster counter */
    c->r_ctr++;
    _co_raster(c);

    if (c->co_raster) {
        /* End of character row */
        c->co_raster = false;
        c->r_ctr     = 0;
        _co_raster(c);

        c->v_ctr++;
        _co_vctr(c);

        /* vdisp wins over vtotal for display enable if they coincide */
        if (c->co_vdisp) {
            c->co_vdisp = false;
            c->v_de     = false;
        }

        if (c->co_vtotal) {
            c->co_vtotal = false;
            /* Check for vertical adjust */
            if (c->v_total_adjust > 0) {
                c->in_vadj   = true;
                c->vadj_ctr  = 0;
            } else {
                /* Immediate new frame */
                c->v_ctr    = 0;
                c->r_ctr    = 0;
                c->v_de     = true;
                c->ma_store = mc6845_get_start_addr(c);
                c->ma_row_start = c->ma_store;
                c->ma       = c->ma_row_start;
                c->frame_count++;
                /* Cursor blink */
                c->cursor_blink_ctr++;
                uint8_t blink_mode = (c->cursor_start >> 5) & 0x03;
                uint8_t blink_rate = (blink_mode == 3) ? 16 : 8;
                if (c->cursor_blink_ctr >= blink_rate) {
                    c->cursor_blink_ctr   = 0;
                    c->cursor_blink_state = !c->cursor_blink_state;
                }
            }
            _co_vctr(c);
        }

        /* VSYNC */
        if (c->co_vspos) {
            c->co_vspos  = false;
            if (!c->vs) {
                c->vs        = true;
                c->vsync_ctr = 0;
                if (c->vsync_cb)
                    c->vsync_cb(c->vsync_ctx, true);
            }
        }

        c->ma_row_start = c->ma_store;

        /* UM6845R: reload ma_row_start on every scanline of row 0 */
        if (c->type == MC6845_TYPE_UM6845R && c->v_ctr == 0) {
            c->ma_store     = mc6845_get_start_addr(c);
            c->ma_row_start = c->ma_store;
        }
    }

    /* VSYNC width countdown */
    if (c->vs) {
        c->vsync_ctr++;
        if (c->vsync_ctr >= _vsync_width(c)) {
            c->vs = false;
            if (c->vsync_cb)
                c->vsync_cb(c->vsync_ctx, false);
        }
    }
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
    c->cursor_on  = false;
    c->cursor_blink_ctr   = 0;
    c->cursor_blink_state = true;
    c->frame_count = 0;
    c->lightpen_latched = false;
    c->co_htotal = c->co_hdisp = c->co_hspos = c->co_hswidth = false;
    c->co_vtotal = c->co_vdisp = c->co_vspos = c->co_vswidth = c->co_raster = false;
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
    /* Advance MA and horizontal counter */
    c->ma    = (c->ma + 1) & 0x3FFF;
    c->h_ctr = c->h_ctr + 1;
    _co_hctr(c);

    /* End of horizontal line? */
    if (c->co_htotal) {
        c->co_htotal = false;
        _advance_vertical(c);
        c->h_de  = true;
        c->h_ctr = 0;
        _co_hctr(c);
        c->ma = c->ma_row_start;
    }

    /* End of horizontal display? */
    if (c->co_hdisp) {
        c->co_hdisp = false;
        c->h_de     = false;
        c->ma_store = c->ma;   /* save MA for start of next character row */
    }

    /* HSYNC start */
    if (c->co_hspos) {
        c->co_hspos  = false;
        c->hs        = true;
        c->hsync_ctr = 0;
    }

    /* HSYNC width countdown */
    if (c->hs) {
        c->co_hswidth = (c->hsync_ctr == _hsync_width(c));
        c->hsync_ctr++;
        if (c->co_hswidth) {
            c->co_hswidth = false;
            c->hs         = false;
        }
    }

    /* ---------- cursor ---------- */
    uint16_t cur_addr = mc6845_get_cursor_addr(c);
    bool cur_h  = (c->ma == cur_addr);
    bool cur_v  = (c->r_ctr >= (c->cursor_start & 0x1F)) &&
                  (c->r_ctr <= (c->cursor_end   & 0x1F));

    bool blink_ok;
    switch ((c->cursor_start >> 5) & 0x03) {
        case 0:  blink_ok = true;  break;       /* no blink: always on  */
        case 1:  blink_ok = false; break;        /* cursor off           */
        default: blink_ok = c->cursor_blink_state; break; /* blink       */
    }
    c->cursor_on = cur_h && cur_v && blink_ok && c->h_de && c->v_de;

    /* ---------- output ---------- */
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
