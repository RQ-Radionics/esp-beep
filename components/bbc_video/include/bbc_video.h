/*
 * bbc_video.h — BBC Micro video subsystem integration
 *
 * Integrates: MC6845 CRTC + Video ULA + SAA5050 Teletext → framebuffer.
 *
 * Usage (frame-at-a-time mode — recommended for ESP32):
 *
 *   bbc_video_t video;
 *   bbc_video_init(&video, system_ram, 32768);
 *
 *   // Configure a 640×256 RGB565 framebuffer in PSRAM:
 *   static uint16_t fb[640 * 256];
 *   bbc_video_output_t out = {
 *       .format     = BBC_FB_FORMAT_RGB565,
 *       .width      = 640,
 *       .height     = 256,
 *       .framebuffer = fb,
 *       .fb_stride  = 640 * 2,
 *   };
 *   bbc_video_set_output(&video, &out);
 *   bbc_video_set_vsync_callback(&video, my_vsync_cb, ctx);
 *   bbc_video_set_frame_callback(&video, my_frame_cb, ctx);
 *
 *   // In the emulator main loop (CPU writes go via bbc_memory callbacks):
 *   bbc_video_crtc_write(&video, addr & 1, data);  // &FE00/&FE01
 *   bbc_video_vidproc_write(&video, addr & 1, data);  // &FE20/&FE21
 *
 *   // Render one complete frame:
 *   bbc_video_render_frame(&video);
 *
 * Licence: zlib
 * Copyright (c) 2026 esp-beep project
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "mc6845.h"
#include "bbc_video_ula.h"
#include "saa5050.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Framebuffer dimensions (native BBC Micro visible area)
 * -------------------------------------------------------------------------- */
#define BBC_FB_WIDTH    640
#define BBC_FB_HEIGHT   256

/* --------------------------------------------------------------------------
 * Framebuffer pixel formats
 * -------------------------------------------------------------------------- */
typedef enum {
    BBC_FB_FORMAT_INDEX8,   /* 1 byte/pixel: physical colour index 0-7       */
    BBC_FB_FORMAT_RGB332,   /* 1 byte/pixel: RRRGGGBB                         */
    BBC_FB_FORMAT_RGB565,   /* 2 bytes/pixel: little-endian RGB565 (LCD SPI) */
    BBC_FB_FORMAT_RGB888,   /* 3 bytes/pixel: R,G,B                           */
} bbc_fb_format_t;

/* --------------------------------------------------------------------------
 * Output configuration
 * -------------------------------------------------------------------------- */
typedef struct {
    bbc_fb_format_t  format;
    uint16_t         width;       /* framebuffer width in pixels              */
    uint16_t         height;      /* framebuffer height in pixels             */
    void            *framebuffer; /* caller-allocated buffer                  */
    uint32_t         fb_stride;   /* bytes per row                            */
} bbc_video_output_t;

/* --------------------------------------------------------------------------
 * BBC Micro screen base addresses (Model B 32 KB)
 * -------------------------------------------------------------------------- */
#define BBC_SCREEN_BASE_MODE012  0x3000u  /* 20 KB: MODEs 0,1,2              */
#define BBC_SCREEN_BASE_MODE3    0x4000u  /* 16 KB: MODE 3                   */
#define BBC_SCREEN_BASE_MODE45   0x5800u  /* 10 KB: MODEs 4,5                */
#define BBC_SCREEN_BASE_MODE6    0x6000u  /*  8 KB: MODE 6                   */
#define BBC_SCREEN_BASE_MODE7    0x7C00u  /*  1 KB: MODE 7 Teletext          */

/* --------------------------------------------------------------------------
 * Full video subsystem state — no heap allocation
 * -------------------------------------------------------------------------- */
typedef struct {
    mc6845_t        crtc;         /* MC6845 CRTC                             */
    bbc_video_ula_t ula;          /* Video ULA                               */
    saa5050_t       teletext;     /* SAA5050                                 */

    /* System RAM pointer (owned by bbc_memory, not by this struct) */
    const uint8_t  *system_ram;
    uint32_t        ram_size;

    /* Output configuration */
    bbc_video_output_t output;

    /* Callbacks */
    void (*vsync_cb)(void *ctx, bool state);
    void  *vsync_ctx;
    void (*frame_cb)(void *ctx);
    void  *frame_ctx;

    /* Statistics */
    uint32_t frames_rendered;
} bbc_video_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void bbc_video_init(bbc_video_t *video,
                     const uint8_t *system_ram, uint32_t ram_size);
void bbc_video_reset(bbc_video_t *video);

/* Configure framebuffer output */
void bbc_video_set_output(bbc_video_t *video,
                           const bbc_video_output_t *output);

/* Connect VSYNC → System VIA CA1 (state: true=assert, false=release) */
void bbc_video_set_vsync_callback(bbc_video_t *video,
                                   void (*cb)(void *ctx, bool state),
                                   void *ctx);

/* Called after each complete frame is rendered into the framebuffer */
void bbc_video_set_frame_callback(bbc_video_t *video,
                                   void (*cb)(void *ctx),
                                   void *ctx);

/* CPU bus access — CRTC (&FE00-&FE01) */
void    bbc_video_crtc_write(bbc_video_t *video, uint8_t addr, uint8_t data);
uint8_t bbc_video_crtc_read (bbc_video_t *video, uint8_t addr);

/* CPU bus access — Video ULA (&FE20-&FE21) */
void bbc_video_vidproc_write(bbc_video_t *video, uint8_t addr, uint8_t data);

/*
 * Tick — advance one CRTC clock cycle.
 *
 * Call from the main CPU loop, once per CRTC clock:
 *   - CRTC at 2 MHz → call every CPU cycle (2 MHz CPU)
 *   - CRTC at 1 MHz → call every other CPU cycle
 *
 * bbc_video_tick() reads video RAM at the MA+RA address output by the CRTC
 * and writes pixels directly to the framebuffer. This is the cycle-exact
 * path (lower throughput but accurate timing).
 */
void bbc_video_tick(bbc_video_t *video);

/*
 * Render a complete frame in one call (frame-at-a-time, non-cycle-exact).
 *
 * Reads the current CRTC and ULA registers, then sweeps through all rows and
 * scanlines, reading video RAM and filling the framebuffer.
 *
 * This is ~10× faster than bbc_video_tick() and sufficient for most
 * display-only use cases on the ESP32.
 *
 * After rendering, fires the frame_cb if installed.
 */
void bbc_video_render_frame(bbc_video_t *video);

/*
 * Flash toggle — call at ~1 Hz (or every 50 calls to a 50 Hz frame tick).
 * Passes through to both the Video ULA and the SAA5050.
 */
void bbc_video_toggle_flash(bbc_video_t *video);

#ifdef __cplusplus
}
#endif
