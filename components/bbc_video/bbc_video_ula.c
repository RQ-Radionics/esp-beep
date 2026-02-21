/*
 * bbc_video_ula.c — Acorn Video ULA emulation for BBC Micro / ESP32-ESP-IDF
 *
 * References: beebwiki.mdfs.net/Video_ULA, B-em video.c (GPL-2, reference only),
 *             BeebFpga vidproc.vhd (reference only).
 *
 * Key implementation notes:
 *   - The palette &FE21 write: bits 7-4 = logical colour, bits 3-0 = physical.
 *     The physical bits are stored XOR'd with 0x07 (BBC hardware quirk).
 *   - Bit interleaving for 2bpp and 4bpp is BBC-specific and different from
 *     a naive MSB-first layout.
 *
 * Licence: zlib
 * Copyright (c) 2026 esp-beep project
 */

#include <string.h>
#include "bbc_video_ula.h"

#ifdef ESP_PLATFORM
#  include "esp_log.h"
#  define ULA_LOGD(fmt, ...) ESP_LOGD("bbc_ula", fmt, ##__VA_ARGS__)
#else
#  define ULA_LOGD(fmt, ...) /* no-op */
#endif

/* --------------------------------------------------------------------------
 * Default palette — MOS 1.20 default: logical colour N → physical colour N
 * for logical colours 0-7; 8-15 are flashing variants (stored as 0-7 with
 * flash bit set in the original ULA, here we just alias to 0-7).
 * -------------------------------------------------------------------------- */
static const uint8_t s_default_palette[16] = {
    /* Logical 0-7: direct 1:1 */
    BBC_COL_BLACK,   /* 0 */
    BBC_COL_RED,     /* 1 */
    BBC_COL_GREEN,   /* 2 */
    BBC_COL_YELLOW,  /* 3 */
    BBC_COL_BLUE,    /* 4 */
    BBC_COL_MAGENTA, /* 5 */
    BBC_COL_CYAN,    /* 6 */
    BBC_COL_WHITE,   /* 7 */
    /* Logical 8-15: same colours (flash handled via flash_state) */
    BBC_COL_BLACK,   /* 8  */
    BBC_COL_RED,     /* 9  */
    BBC_COL_GREEN,   /* 10 */
    BBC_COL_YELLOW,  /* 11 */
    BBC_COL_BLUE,    /* 12 */
    BBC_COL_MAGENTA, /* 13 */
    BBC_COL_CYAN,    /* 14 */
    BBC_COL_WHITE,   /* 15 */
};

/* --------------------------------------------------------------------------
 * Colour table: physical colour (0-7, BBB_COL_*) → RGB
 * BBC uses 3-bit RGB: bit 0=R, bit 1=G, bit 2=B
 * -------------------------------------------------------------------------- */
static const bbc_rgb_t s_colour_table[8] = {
    { 0,   0,   0   }, /* 0: Black   */
    { 255, 0,   0   }, /* 1: Red     */
    { 0,   255, 0   }, /* 2: Green   */
    { 255, 255, 0   }, /* 3: Yellow  */
    { 0,   0,   255 }, /* 4: Blue    */
    { 255, 0,   255 }, /* 5: Magenta */
    { 0,   255, 255 }, /* 6: Cyan    */
    { 255, 255, 255 }, /* 7: White   */
};

/* --------------------------------------------------------------------------
 * Rebuild lookup tables after any palette change
 * -------------------------------------------------------------------------- */
void bbc_video_ula_rebuild_tables(bbc_video_ula_t *ula)
{
    /*
     * 1bpp (MODE 0, 3, 4, 6): 8 pixels per byte.
     * Pixel i uses bit (7-i) of the byte.
     * logical colour = that single bit → palette[bit].
     */
    for (int byte = 0; byte < 256; byte++) {
        for (int px = 0; px < 8; px++) {
            uint8_t bit = (byte >> (7 - px)) & 1;
            ula->lut_1bpp[byte][px] = ula->palette[bit];
        }
    }

    /*
     * 2bpp (MODE 1, 5): 4 pixels per byte.
     * Each pixel uses two bits: the high bit from the upper nibble positions,
     * the low bit from the lower nibble positions.
     *
     * BBC bit ordering:
     *   Pixel 0: {bit7, bit3}
     *   Pixel 1: {bit6, bit2}
     *   Pixel 2: {bit5, bit1}
     *   Pixel 3: {bit4, bit0}
     *
     * This is an interleaved layout — the two bits of each pixel are 4
     * positions apart. logical colour = 0-3.
     */
    for (int byte = 0; byte < 256; byte++) {
        for (int px = 0; px < 4; px++) {
            uint8_t high = (byte >> (7 - px)) & 1;  /* bits 7,6,5,4 */
            uint8_t low  = (byte >> (3 - px)) & 1;  /* bits 3,2,1,0 */
            uint8_t logical = (high << 1) | low;
            ula->lut_2bpp[byte][px] = ula->palette[logical];
        }
    }

    /*
     * 4bpp (MODE 2): 2 pixels per byte.
     * Each pixel uses 4 bits: positions {7,5,3,1} for pixel 0
     *                         and {6,4,2,0} for pixel 1.
     *
     * BBC bit ordering (from B-em table4bpp initialisation):
     *   Pixel 0: {bit7, bit5, bit3, bit1}  (even-offset bits)
     *   Pixel 1: {bit6, bit4, bit2, bit0}  (odd-offset bits)
     *
     * logical colour = 0-15.
     */
    for (int byte = 0; byte < 256; byte++) {
        for (int px = 0; px < 2; px++) {
            uint8_t b3 = (byte >> (7 - px)) & 1;
            uint8_t b2 = (byte >> (5 - px)) & 1;
            uint8_t b1 = (byte >> (3 - px)) & 1;
            uint8_t b0 = (byte >> (1 - px)) & 1;
            uint8_t logical = (b3 << 3) | (b2 << 2) | (b1 << 1) | b0;
            ula->lut_4bpp[byte][px] = ula->palette[logical];
        }
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void bbc_video_ula_init(bbc_video_ula_t *ula)
{
    memset(ula, 0, sizeof(*ula));
    /* Install colour table */
    for (int i = 0; i < 8; i++)
        ula->colour_table[i] = s_colour_table[i];
    bbc_video_ula_reset(ula);
}

void bbc_video_ula_reset(bbc_video_ula_t *ula)
{
    ula->control       = 0;
    ula->teletext_mode = false;
    ula->bpp_mode      = ULA_BPP_1;
    ula->pixels_per_byte = 8;
    ula->crtc_2mhz     = false;
    ula->flash_state   = false;

    /* Copy default palette */
    for (int i = 0; i < 16; i++)
        ula->palette[i] = s_default_palette[i];

    bbc_video_ula_rebuild_tables(ula);
    ULA_LOGD("reset");
}

void bbc_video_ula_write(bbc_video_ula_t *ula, uint8_t addr, uint8_t data)
{
    if (!(addr & 1)) {
        /*
         * &FE20 — Control register
         *
         * Bit 0: flash colour state (MOS toggles this ~1 Hz)
         * Bit 1: teletext mode select
         * Bits 3-2: chars-per-pixel-byte → bpp mode
         * Bit 4: CRTC 2 MHz (1 = 2 MHz, 0 = 1 MHz)
         * Bit 7: flash colour control
         */
        ula->control     = data;
        ula->flash_state = (data & ULA_CTRL_FLASH_STATE) != 0;
        ula->teletext_mode = (data & ULA_CTRL_TELETEXT) != 0;
        ula->crtc_2mhz   = (data & ULA_CTRL_CRTC_2MHZ) != 0;

        uint8_t bpp_field = (data & ULA_CTRL_BPP_MASK) >> ULA_CTRL_BPP_SHIFT;
        ula->bpp_mode = bpp_field;
        switch (bpp_field) {
            case ULA_BPP_1: ula->pixels_per_byte = 8; break;
            case ULA_BPP_2: ula->pixels_per_byte = 4; break;
            case ULA_BPP_4: ula->pixels_per_byte = 2; break;
            default:        ula->pixels_per_byte = 1; break;
        }
        /* Rebuild tables as flash_state affects palette output */
        bbc_video_ula_rebuild_tables(ula);
        ULA_LOGD("ctrl=%02X teletext=%d bpp=%d 2mhz=%d",
                 data, ula->teletext_mode, ula->pixels_per_byte, ula->crtc_2mhz);
    } else {
        /*
         * &FE21 — Palette register
         *
         * Bits 7-4: logical colour index (0-15)
         * Bits 3-0: physical colour data
         *
         * Hardware quirk: the physical colour bits are inverted (XOR 7)
         * before being stored. So physical = (data & 0x07) ^ 0x07.
         * This means writing 0 to bits 2-0 stores white, writing 7 stores black.
         */
        uint8_t logical  = (data >> 4) & 0x0F;
        uint8_t physical = (data & 0x07) ^ 0x07;
        ula->palette[logical] = physical;
        bbc_video_ula_rebuild_tables(ula);
        ULA_LOGD("palette[%d] = %d (raw=%02X)", logical, physical, data);
    }
}

int bbc_video_ula_serialize(const bbc_video_ula_t *ula,
                             uint8_t data_byte,
                             uint8_t *out_colours,
                             bool cursor_active)
{
    int n;
    switch (ula->bpp_mode) {
        case ULA_BPP_1:
            n = 8;
            for (int i = 0; i < 8; i++)
                out_colours[i] = ula->lut_1bpp[data_byte][i];
            break;
        case ULA_BPP_2:
            n = 4;
            for (int i = 0; i < 4; i++)
                out_colours[i] = ula->lut_2bpp[data_byte][i];
            break;
        case ULA_BPP_4:
            n = 2;
            for (int i = 0; i < 2; i++)
                out_colours[i] = ula->lut_4bpp[data_byte][i];
            break;
        default:
            n = 1;
            out_colours[0] = ula->palette[data_byte & 0x0F];
            break;
    }

    /* Cursor: XOR physical colour with 7 = invert all 3 bits */
    if (cursor_active) {
        for (int i = 0; i < n; i++)
            out_colours[i] ^= 7;
    }
    return n;
}

void bbc_video_ula_toggle_flash(bbc_video_ula_t *ula)
{
    ula->flash_state = !ula->flash_state;
    ula->control ^= ULA_CTRL_FLASH_STATE;
    bbc_video_ula_rebuild_tables(ula);
    ULA_LOGD("flash=%d", ula->flash_state);
}
