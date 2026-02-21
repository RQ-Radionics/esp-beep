/*
 * bbc_video.c — BBC Micro video subsystem integration
 *
 * Integrates MC6845 CRTC + Video ULA + SAA5050 → framebuffer.
 *
 * MA+RA → RAM address (BBC Micro bitmap modes):
 *   ram_addr = ((MA & 0x1FFF) | ((RA & 0x07) << 13)) & 0x7FFF
 *
 * This creates a "rasterised" layout where scanlines are interleaved.
 *
 * Licence: zlib
 * Copyright (c) 2026 esp-beep project
 */

#include <string.h>
#include "bbc_video.h"

#ifdef ESP_PLATFORM
#  include "esp_log.h"
#  define VID_LOGD(fmt, ...) ESP_LOGD("bbc_video", fmt, ##__VA_ARGS__)
#  define VID_LOGW(fmt, ...) ESP_LOGW("bbc_video", fmt, ##__VA_ARGS__)
#else
#  include <stdio.h>
#  define VID_LOGD(fmt, ...) /* no-op */
#  define VID_LOGW(fmt, ...) fprintf(stderr, "bbc_video WARN: " fmt "\n", ##__VA_ARGS__)
#endif

/* --------------------------------------------------------------------------
 * Framebuffer pixel write helpers
 * -------------------------------------------------------------------------- */

static inline void write_pixel(uint8_t *fb, uint32_t stride,
                                int x, int y,
                                bbc_rgb_t rgb,
                                bbc_fb_format_t fmt)
{
    switch (fmt) {
    case BBC_FB_FORMAT_INDEX8: {
        uint8_t idx = (uint8_t)(((rgb.b ? 1 : 0) << 2) |
                                ((rgb.g ? 1 : 0) << 1) |
                                 (rgb.r ? 1 : 0));
        fb[y * stride + x] = idx;
        break;
    }
    case BBC_FB_FORMAT_RGB332: {
        uint8_t v = (rgb.r & 0xE0) | ((rgb.g >> 3) & 0x1C) | (rgb.b >> 6);
        fb[y * stride + x] = v;
        break;
    }
    case BBC_FB_FORMAT_RGB565: {
        uint16_t r5 = (rgb.r >> 3) & 0x1F;
        uint16_t g6 = (rgb.g >> 2) & 0x3F;
        uint16_t b5 = (rgb.b >> 3) & 0x1F;
        uint16_t pix = (r5 << 11) | (g6 << 5) | b5;
        uint8_t *p = fb + y * stride + x * 2;
        p[0] = pix & 0xFF;
        p[1] = (pix >> 8) & 0xFF;
        break;
    }
    case BBC_FB_FORMAT_RGB888: {
        uint8_t *p = fb + y * stride + x * 3;
        p[0] = rgb.r;
        p[1] = rgb.g;
        p[2] = rgb.b;
        break;
    }
    }
}

/* --------------------------------------------------------------------------
 * BBC Micro MA+RA → RAM address bit-shuffle
 * -------------------------------------------------------------------------- */
static inline uint32_t bbc_bitmap_ram_addr(uint16_t ma, uint8_t ra)
{
    return ((uint32_t)(ma & 0x1FFF) | ((uint32_t)(ra & 0x07) << 13)) & 0x7FFF;
}

/* --------------------------------------------------------------------------
 * VSYNC callback bridge
 * -------------------------------------------------------------------------- */
static void _vsync_cb(void *ctx, bool state)
{
    bbc_video_t *video = (bbc_video_t *)ctx;
    if (video->vsync_cb)
        video->vsync_cb(video->vsync_ctx, state);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void bbc_video_init(bbc_video_t *video,
                     const uint8_t *system_ram, uint32_t ram_size)
{
    memset(video, 0, sizeof(*video));
    video->system_ram = system_ram;
    video->ram_size   = ram_size;

    mc6845_init(&video->crtc, MC6845_TYPE_MC6845);
    mc6845_set_vsync_callback(&video->crtc, _vsync_cb, video);

    bbc_video_ula_init(&video->ula);
    saa5050_init(&video->teletext, NULL);

    VID_LOGD("init: ram=%p size=%lu", (const void *)system_ram,
             (unsigned long)ram_size);
}

void bbc_video_reset(bbc_video_t *video)
{
    mc6845_reset(&video->crtc);
    bbc_video_ula_reset(&video->ula);
    saa5050_reset(&video->teletext);
    video->frames_rendered = 0;
    VID_LOGD("reset");
}

void bbc_video_set_output(bbc_video_t *video,
                           const bbc_video_output_t *output)
{
    video->output = *output;
}

void bbc_video_set_vsync_callback(bbc_video_t *video,
                                   void (*cb)(void *ctx, bool state),
                                   void *ctx)
{
    video->vsync_cb  = cb;
    video->vsync_ctx = ctx;
}

void bbc_video_set_frame_callback(bbc_video_t *video,
                                   void (*cb)(void *ctx),
                                   void *ctx)
{
    video->frame_cb  = cb;
    video->frame_ctx = ctx;
}

void bbc_video_crtc_write(bbc_video_t *video, uint8_t addr, uint8_t data)
{
    mc6845_write(&video->crtc, addr, data);
}

uint8_t bbc_video_crtc_read(bbc_video_t *video, uint8_t addr)
{
    return mc6845_read(&video->crtc, addr);
}

void bbc_video_vidproc_write(bbc_video_t *video, uint8_t addr, uint8_t data)
{
    bbc_video_ula_write(&video->ula, addr, data);
}

/* --------------------------------------------------------------------------
 * Cycle-exact tick (called once per CRTC clock from the main loop)
 * -------------------------------------------------------------------------- */
void bbc_video_tick(bbc_video_t *video)
{
    if (!video->output.framebuffer || !video->system_ram) return;

    mc6845_output_t out = mc6845_tick(&video->crtc);
    if (!out.display_enable) return;

    const mc6845_t *crtc = &video->crtc;
    uint8_t *fb = (uint8_t *)video->output.framebuffer;

    /* Approximate screen position — not perfectly cycle-exact but close */
    int char_row  = (int)crtc->v_ctr;
    int scan_line = (int)crtc->r_ctr;
    int char_col  = (int)crtc->h_ctr;
    int scans     = (int)(crtc->max_scanline_addr + 1);
    int h_disp    = (int)crtc->h_displayed;
    int v_disp    = (int)crtc->v_displayed;

    if (h_disp == 0 || v_disp == 0) return;

    int out_y = (char_row * scans + scan_line) * (int)video->output.height
                / (v_disp * scans);
    if (out_y < 0 || out_y >= (int)video->output.height) return;

    if (video->ula.teletext_mode) {
        uint32_t ram_addr = (BBC_SCREEN_BASE_MODE7 + (uint32_t)(char_row * SAA5050_COLS + char_col)) & 0x7FFF;
        if (ram_addr >= video->ram_size) return;
        uint8_t code = video->system_ram[ram_addr] & 0x7F;

        saa5050_line_state_t ls;
        saa5050_start_scanline(&video->teletext, &ls, (uint8_t)(scan_line * 2));
        uint8_t pixels[SAA5050_PIXELS_PER_CHAR];
        saa5050_render_char(&video->teletext, &ls, code, pixels);

        int base_x = char_col * (int)video->output.width / SAA5050_COLS;
        for (int i = 0; i < SAA5050_PIXELS_PER_CHAR; i++) {
            int fx = base_x + i * (int)video->output.width / (SAA5050_COLS * SAA5050_PIXELS_PER_CHAR);
            if (fx >= (int)video->output.width) break;
            bbc_rgb_t rgb = bbc_video_ula_colour(&video->ula, pixels[i]);
            write_pixel(fb, video->output.fb_stride, fx, out_y, rgb,
                        video->output.format);
        }
    } else {
        uint32_t ram_addr = bbc_bitmap_ram_addr(out.ma, out.ra);
        if (ram_addr >= video->ram_size) return;
        uint8_t data_byte = video->system_ram[ram_addr];

        uint8_t colours[8];
        int npx = bbc_video_ula_serialize(&video->ula, data_byte,
                                          colours, out.cursor);
        int pixel_width = video->ula.crtc_2mhz ? 1 : 2;
        int base_x = char_col * npx * pixel_width;
        for (int px = 0; px < npx; px++) {
            bbc_rgb_t rgb = bbc_video_ula_colour(&video->ula, colours[px]);
            int fx = base_x + px * pixel_width;
            for (int d = 0; d < pixel_width; d++) {
                int out_x = fx + d;
                if (out_x >= (int)video->output.width) break;
                write_pixel(fb, video->output.fb_stride, out_x, out_y, rgb,
                            video->output.format);
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * Frame-at-a-time rendering (recommended for ESP32)
 * -------------------------------------------------------------------------- */

static void render_bitmap_frame(bbc_video_t *video)
{
    const mc6845_t      *crtc = &video->crtc;
    const bbc_video_ula_t *ula  = &video->ula;
    uint8_t *fb = (uint8_t *)video->output.framebuffer;
    uint32_t stride = video->output.fb_stride;
    bbc_fb_format_t fmt = video->output.format;

    uint16_t start_addr     = mc6845_get_start_addr(crtc);
    int chars_per_line      = (int)crtc->h_displayed;
    int char_rows           = (int)crtc->v_displayed;
    int scans_per_char      = (int)(crtc->max_scanline_addr + 1);
    uint16_t cursor_addr    = mc6845_get_cursor_addr(crtc);
    int cursor_start        = (int)(crtc->cursor_start & 0x1F);
    int cursor_end          = (int)(crtc->cursor_end   & 0x1F);
    int pixel_width         = ula->crtc_2mhz ? 1 : 2;
    int total_scanlines     = char_rows * scans_per_char;

    if (chars_per_line == 0 || char_rows == 0 || total_scanlines == 0) return;

    for (int row = 0; row < char_rows; row++) {
        for (int sl = 0; sl < scans_per_char; sl++) {
            int out_y = (row * scans_per_char + sl) * (int)video->output.height
                        / total_scanlines;
            if (out_y < 0 || out_y >= (int)video->output.height) continue;

            for (int col = 0; col < chars_per_line; col++) {
                uint16_t ma = (uint16_t)((start_addr + row * chars_per_line + col) & 0x3FFF);
                uint32_t ram_addr = bbc_bitmap_ram_addr(ma, (uint8_t)sl);
                if (ram_addr >= video->ram_size) continue;

                uint8_t data_byte = video->system_ram[ram_addr];

                bool cursor = (ma == cursor_addr) &&
                              (sl >= cursor_start) && (sl <= cursor_end) &&
                              crtc->cursor_blink_state;

                uint8_t colours[8];
                int npx = bbc_video_ula_serialize(ula, data_byte, colours, cursor);

                int base_x = col * npx * pixel_width;
                for (int px = 0; px < npx; px++) {
                    bbc_rgb_t rgb = bbc_video_ula_colour(ula, colours[px]);
                    int fx = base_x + px * pixel_width;
                    for (int d = 0; d < pixel_width; d++) {
                        int out_x = fx + d;
                        if (out_x >= (int)video->output.width) break;
                        write_pixel(fb, stride, out_x, out_y, rgb, fmt);
                    }
                }
            }
        }
    }
}

static void render_teletext_frame(bbc_video_t *video)
{
    saa5050_t           *tt  = &video->teletext;
    const bbc_video_ula_t *ula = &video->ula;
    uint8_t *fb = (uint8_t *)video->output.framebuffer;
    uint32_t stride = video->output.fb_stride;
    bbc_fb_format_t fmt = video->output.format;

    /* SAA5050 output: 40 cols × 12 px = 480 px wide, 25 rows × 20 sl = 500 sl tall.
     * Centre horizontally in the framebuffer (typically 640 px wide).
     * Map 500 logical scanlines → output height (typically 256), clamped. */
    int content_w  = SAA5050_COLS * SAA5050_PIXELS_PER_CHAR; /* 480 */
    int x_offset   = ((int)video->output.width - content_w) / 2;
    if (x_offset < 0) x_offset = 0;

    int total_scanlines = SAA5050_ROWS * SAA5050_SCANLINES_PER_ROW; /* 500 */

    /* Clear framebuffer to background colour (index 0 = black) before rendering */
    memset(fb, 0, (size_t)stride * video->output.height);

    for (int row = 0; row < SAA5050_ROWS; row++) {
        saa5050_start_row(tt, (uint8_t)row);

        for (int sl = 0; sl < SAA5050_SCANLINES_PER_ROW; sl++) {
            int out_y = (row * SAA5050_SCANLINES_PER_ROW + sl)
                        * (int)video->output.height / total_scanlines;
            if (out_y < 0 || out_y >= (int)video->output.height) continue;

            saa5050_line_state_t ls;
            saa5050_start_scanline(tt, &ls, (uint8_t)sl);

            for (int col = 0; col < SAA5050_COLS; col++) {
                uint32_t ram_addr = (BBC_SCREEN_BASE_MODE7 +
                                     (uint32_t)(row * SAA5050_COLS + col)) & 0x7FFF;
                uint8_t code = 0x20;
                if (ram_addr < video->ram_size)
                    code = video->system_ram[ram_addr] & 0x7F;

                uint8_t pixels[SAA5050_PIXELS_PER_CHAR];
                saa5050_render_char(tt, &ls, code, pixels);

                /* Pixel-exact: each character occupies exactly SAA5050_PIXELS_PER_CHAR
                 * (12) output pixels, placed at x_offset + col * 12. */
                int base_x = x_offset + col * SAA5050_PIXELS_PER_CHAR;

                for (int px = 0; px < SAA5050_PIXELS_PER_CHAR; px++) {
                    int fx = base_x + px;
                    if (fx < 0 || fx >= (int)video->output.width) continue;
                    bbc_rgb_t rgb = bbc_video_ula_colour(ula, pixels[px]);
                    write_pixel(fb, stride, fx, out_y, rgb, fmt);
                }
            }
        }
    }
}

void bbc_video_render_frame(bbc_video_t *video)
{
    if (!video->output.framebuffer || !video->system_ram) {
        VID_LOGW("render_frame: no framebuffer or RAM");
        return;
    }

    if (video->ula.teletext_mode)
        render_teletext_frame(video);
    else
        render_bitmap_frame(video);

    video->frames_rendered++;

    if (video->frame_cb)
        video->frame_cb(video->frame_ctx);
}

void bbc_video_toggle_flash(bbc_video_t *video)
{
    bbc_video_ula_toggle_flash(&video->ula);
    saa5050_toggle_flash(&video->teletext);
}
