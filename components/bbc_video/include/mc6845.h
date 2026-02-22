/*
 * mc6845.h — Motorola MC6845 CRTC emulation for BBC Micro / ESP32-ESP-IDF
 *
 * Derived from floooh/chips mc6845.h by Andre Weissflog (zlib licence).
 * Adapted for ESP-IDF: pin-bus model replaced by a direct register/callback
 * API. Cursor logic, VSYNC callback, and BBC-specific type added.
 * All state in mc6845_t — no globals, no malloc.
 *
 * Licence: zlib (same as original)
 * Copyright (c) 2018 Andre Weissflog
 * Adaptation (c) 2026 esp-beep project
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Register indices (select via mc6845_select_reg, access via _write/_read)
 * -------------------------------------------------------------------------- */
#define MC6845_R0_HTOTAL         0   /* Horizontal Total (chars - 1)         */
#define MC6845_R1_HDISPLAYED     1   /* Horizontal Displayed (chars)         */
#define MC6845_R2_HSYNCPOS       2   /* Horizontal Sync Position             */
#define MC6845_R3_SYNCWIDTHS     3   /* H/V Sync Widths (nibbles)            */
#define MC6845_R4_VTOTAL         4   /* Vertical Total (rows - 1)            */
#define MC6845_R5_VTOTALADJ      5   /* Vertical Total Adjust (scanlines)    */
#define MC6845_R6_VDISPLAYED     6   /* Vertical Displayed (rows)            */
#define MC6845_R7_VSYNCPOS       7   /* Vertical Sync Position (row)         */
#define MC6845_R8_INTERLACE      8   /* Interlace and Skew Mode              */
#define MC6845_R9_MAXSCANLINE    9   /* Max Scanline Address (scanlines - 1) */
#define MC6845_R10_CURSORSTART  10   /* Cursor Start + blink mode            */
#define MC6845_R11_CURSOREND    11   /* Cursor End scanline                  */
#define MC6845_R12_STARTHI      12   /* Start Address High (6 bits)          */
#define MC6845_R13_STARTLO      13   /* Start Address Low  (8 bits)          */
#define MC6845_R14_CURSORHI     14   /* Cursor Address High (R/W)            */
#define MC6845_R15_CURSORLO     15   /* Cursor Address Low  (R/W)            */
#define MC6845_R16_LIGHTPENHI   16   /* Light Pen Address High (R only)      */
#define MC6845_R17_LIGHTPENLO   17   /* Light Pen Address Low  (R only)      */

/* --------------------------------------------------------------------------
 * CRTC chip sub-types
 * -------------------------------------------------------------------------- */
typedef enum {
    MC6845_TYPE_UM6845  = 0,    /* UMC UM6845 (CRTC type 0) — Amstrad CPC  */
    MC6845_TYPE_UM6845R = 1,    /* UMC UM6845R (CRTC type 1) — Amstrad CPC */
    MC6845_TYPE_MC6845  = 2,    /* Motorola MC6845 (CRTC type 2) — BBC Micro*/
} mc6845_type_t;

/* --------------------------------------------------------------------------
 * Output signals from one CRTC tick
 * -------------------------------------------------------------------------- */
typedef struct {
    uint16_t ma;             /* Memory Address output (14 bits, MA0-MA13)    */
    uint8_t  ra;             /* Row Address (5 bits, RA0-RA4)                */
    bool     display_enable; /* true = inside visible display area           */
    bool     hsync;          /* Horizontal sync active                       */
    bool     vsync;          /* Vertical sync active                         */
    bool     cursor;         /* Cursor active at this position/scanline      */
} mc6845_output_t;

/* --------------------------------------------------------------------------
 * Full CRTC state — allocate statically, no heap needed
 * -------------------------------------------------------------------------- */
typedef struct {
    mc6845_type_t type;

    /* Address register (selects which of R0-R17 is active) */
    uint8_t sel;

    /* Register bank (R0-R17, plus padding to 32 for UM6845R reg-31 trick) */
    union {
        uint8_t reg[32];
        struct {
            uint8_t h_total;          /* R0  */
            uint8_t h_displayed;      /* R1  */
            uint8_t h_sync_pos;       /* R2  */
            uint8_t sync_widths;      /* R3  */
            uint8_t v_total;          /* R4  */
            uint8_t v_total_adjust;   /* R5  */
            uint8_t v_displayed;      /* R6  */
            uint8_t v_sync_pos;       /* R7  */
            uint8_t interlace_mode;   /* R8  */
            uint8_t max_scanline_addr;/* R9  */
            uint8_t cursor_start;     /* R10 */
            uint8_t cursor_end;       /* R11 */
            uint8_t start_addr_hi;    /* R12 */
            uint8_t start_addr_lo;    /* R13 */
            uint8_t cursor_hi;        /* R14 */
            uint8_t cursor_lo;        /* R15 */
            uint8_t lightpen_hi;      /* R16 */
            uint8_t lightpen_lo;      /* R17 */
        };
    };

    /* Internal counters */
    uint8_t  h_ctr;         /* Horizontal character counter                  */
    uint8_t  hsync_ctr;     /* HSYNC width counter                           */
    uint8_t  v_ctr;         /* Vertical character-row counter                */
    uint8_t  r_ctr;         /* Raster (scanline within char row) counter     */
    uint8_t  vsync_ctr;     /* VSYNC width counter                           */
    uint8_t  vadj_ctr;      /* Vertical adjust counter                       */

    /* Memory address tracking */
    uint16_t ma;            /* Current memory address output                 */
    uint16_t ma_row_start;  /* MA at start of current character row          */
    uint16_t ma_store;      /* Intermediate MA latch                         */

    /* Display enable flags */
    bool     h_de;          /* Horizontal display enable                     */
    bool     v_de;          /* Vertical display enable                       */

    /* Sync flags */
    bool     hs;            /* HSYNC active                                  */
    bool     vs;            /* VSYNC active                                  */

    /* Vertical adjust phase */
    bool     in_vadj;       /* True while processing vertical adjust scanlines*/

    /* Interlace / field state */
    bool     odd_field;       /* Odd field flag (interlace mode)              */

    /* Cursor */
    bool     cursor_on;       /* Cursor visible at current MA/RA             */
    bool     cursor_line_ff;  /* VHDL cursor_line variable — persists across scanlines */
    uint8_t  cursor_blink_ctr; /* Frame counter for cursor blink             */
    bool     cursor_blink_state; /* Current blink state (true = visible)     */
    uint16_t frame_count;   /* Total frames elapsed                          */

    /* Light pen */
    uint16_t lightpen_addr; /* Latched address when light pen strobed        */
    bool     lightpen_latched;

    /* VSYNC edge callback → connects to System VIA CA1 */
    void (*vsync_cb)(void *ctx, bool state);
    void  *vsync_ctx;
} mc6845_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/* Initialise; type selects MC6845 variant (use MC6845_TYPE_MC6845 for BBC) */
void mc6845_init(mc6845_t *crtc, mc6845_type_t type);

/* Hardware reset — clears counters, output low; registers unchanged */
void mc6845_reset(mc6845_t *crtc);

/* CPU register access — addr bit 0 selects address-reg (0) vs data-reg (1) */
void    mc6845_write(mc6845_t *crtc, uint8_t addr, uint8_t data);
uint8_t mc6845_read (mc6845_t *crtc, uint8_t addr);

/*
 * Clock tick — advance one CRTC clock cycle (1 or 2 MHz on BBC depending
 * on Video ULA control register bit 4).
 *
 * Returns the output signals for this cycle (MA, RA, DE, HSYNC, VSYNC,
 * CURSOR). The caller (bbc_video) uses MA+RA to fetch video RAM bytes and
 * display_enable to gate pixel output.
 */
mc6845_output_t mc6845_tick(mc6845_t *crtc);

/* Light pen strobe — latches current MA into R16/R17 */
void mc6845_light_pen_strobe(mc6845_t *crtc);

/* Install VSYNC callback (fires on VSYNC rising and falling edge) */
void mc6845_set_vsync_callback(mc6845_t *crtc,
                                void (*cb)(void *ctx, bool state),
                                void *ctx);

/* Helpers */
static inline uint16_t mc6845_get_start_addr(const mc6845_t *crtc) {
    return ((uint16_t)(crtc->start_addr_hi & 0x3F) << 8) | crtc->start_addr_lo;
}
static inline uint16_t mc6845_get_cursor_addr(const mc6845_t *crtc) {
    return ((uint16_t)(crtc->cursor_hi & 0x3F) << 8) | crtc->cursor_lo;
}

#ifdef __cplusplus
}
#endif
