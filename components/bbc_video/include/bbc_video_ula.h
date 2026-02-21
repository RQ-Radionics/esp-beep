/*
 * bbc_video_ula.h — Acorn Video ULA (5C094/5C095) emulation
 *
 * The Video ULA is a custom Acorn chip that:
 *   - Generates system clocks (16 MHz → 8/4/2/1 MHz)
 *   - Provides the CRTC clock (1 or 2 MHz, selecting 40 or 80 cols)
 *   - Serialises video RAM bytes → pixels using BBC-specific bit interleaving
 *   - Holds a 16-entry palette (logical colour → 3-bit physical colour)
 *   - Handles flashing colours (~1 Hz toggle via MOS interrupt)
 *
 * References: beebwiki.mdfs.net/Video_ULA, B-em video.c (GPL-2, reference only)
 * Licence: zlib
 * Copyright (c) 2026 esp-beep project
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Physical colour indices (3 bits: B2=Blue, B1=Green, B0=Red)
 * -------------------------------------------------------------------------- */
typedef enum {
    BBC_COL_BLACK   = 0,
    BBC_COL_RED     = 1,
    BBC_COL_GREEN   = 2,
    BBC_COL_YELLOW  = 3,
    BBC_COL_BLUE    = 4,
    BBC_COL_MAGENTA = 5,
    BBC_COL_CYAN    = 6,
    BBC_COL_WHITE   = 7,
} bbc_colour_t;

/* --------------------------------------------------------------------------
 * Pixel format for the framebuffer
 * -------------------------------------------------------------------------- */
typedef struct {
    uint8_t r, g, b;   /* 0 or 255 */
} bbc_rgb_t;

/* --------------------------------------------------------------------------
 * Control register (&FE20) bit definitions
 * -------------------------------------------------------------------------- */
#define ULA_CTRL_FLASH_STATE    0x01  /* bit 0: current flash colour select  */
#define ULA_CTRL_TELETEXT       0x02  /* bit 1: teletext mode (SAA5050)      */
#define ULA_CTRL_BPP_MASK       0x0C  /* bits 3-2: chars-per-pixel-byte      */
#define ULA_CTRL_BPP_SHIFT      2
#define ULA_CTRL_CRTC_2MHZ      0x10  /* bit 4: CRTC clock = 2 MHz (80 col)  */
#define ULA_CTRL_CURSOR_MASK    0x30  /* bits 5-4: cursor lines (overlap)    */
#define ULA_CTRL_FLASH_CTRL     0x80  /* bit 7: flash colour control         */

/* bpp field encoding (bits 3-2 of control register) */
#define ULA_BPP_1   0   /* 8 pixels/byte, 2 logical colours  (MODE 0,3,4,6) */
#define ULA_BPP_2   1   /* 4 pixels/byte, 4 logical colours  (MODE 1,5)     */
#define ULA_BPP_4   2   /* 2 pixels/byte, 16 logical colours (MODE 2)       */
#define ULA_BPP_8   3   /* 1 pixel/byte  (undocumented, not used by MOS)    */

/* --------------------------------------------------------------------------
 * Video ULA state — no heap allocation
 * -------------------------------------------------------------------------- */
typedef struct {
    /* Control register (&FE20) */
    uint8_t control;

    /* Decoded fields */
    bool     teletext_mode;   /* true = SAA5050 output (MODE 7)              */
    uint8_t  bpp_mode;        /* ULA_BPP_1/2/4/8                            */
    uint8_t  pixels_per_byte; /* 8, 4, 2, or 1                              */
    bool     crtc_2mhz;       /* true = 2 MHz CRTC (80 chars/line)           */
    bool     flash_state;     /* current flash colour select (bit 0)         */

    /*
     * Palette: 16 entries mapping logical colour → physical colour (0-7).
     * Writes to &FE21 set palette[code >> 4] = (data & 0x07) ^ 0x07.
     * The XOR-inversion is a BBC hardware quirk.
     */
    uint8_t  palette[16];

    /*
     * Lookup tables for fast byte→pixel serialisation.
     * Built by bbc_video_ula_rebuild_tables() after any palette change.
     *
     * byte_to_pixels_Nbpp[byte][pixel_index] = physical colour (0-7)
     */
    uint8_t  lut_1bpp[256][8];   /* 1bpp: 8 pixels per byte                 */
    uint8_t  lut_2bpp[256][4];   /* 2bpp: 4 pixels per byte                 */
    uint8_t  lut_4bpp[256][2];   /* 4bpp: 2 pixels per byte                 */

    /* RGB values for the 8 physical colours */
    bbc_rgb_t colour_table[8];
} bbc_video_ula_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void bbc_video_ula_init(bbc_video_ula_t *ula);
void bbc_video_ula_reset(bbc_video_ula_t *ula);

/* CPU writes:
 *   addr & 1 == 0 → &FE20 Control register
 *   addr & 1 == 1 → &FE21 Palette register  */
void bbc_video_ula_write(bbc_video_ula_t *ula, uint8_t addr, uint8_t data);

/*
 * Serialise one video RAM byte into physical colour indices.
 *
 * data_byte:     the byte read from video RAM
 * out_colours:   caller-provided array (must be ≥ pixels_per_byte entries)
 * cursor_active: true → XOR each pixel colour with 7 (invert = cursor)
 *
 * Returns the number of pixels written (= pixels_per_byte).
 */
int bbc_video_ula_serialize(const bbc_video_ula_t *ula,
                             uint8_t data_byte,
                             uint8_t *out_colours,
                             bool cursor_active);

/*
 * Toggle flash state (~1 Hz, called from MOS 100 Hz interrupt every 50
 * ticks).  Rebuilds palette lookup tables if needed.
 */
void bbc_video_ula_toggle_flash(bbc_video_ula_t *ula);

/* Rebuild lookup tables — called internally after palette/control changes */
void bbc_video_ula_rebuild_tables(bbc_video_ula_t *ula);

/* Map physical colour (0-7) to RGB */
static inline bbc_rgb_t bbc_video_ula_colour(const bbc_video_ula_t *ula,
                                              uint8_t phys) {
    return ula->colour_table[phys & 7];
}

#ifdef __cplusplus
}
#endif
