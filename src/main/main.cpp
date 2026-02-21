/*
 * main.cpp — ESP32 BBC Micro emulator entry point
 *
 * Hardware target: Olimex ESP32-SBC-FabGL (ESP32-WROVER-E, 4 MB Flash + 8 MB PSRAM)
 * Pin assignments: see board.h
 *
 * Display: FabGL VGADirectController (64-colour, 8 GPIOs)
 *   - No persistent viewport — only 2 DMA scan-line buffers allocated by FabGL.
 *   - A 320×256 index-8 back-buffer (one byte per pixel, BBC colour 0-7)
 *     is allocated in PSRAM.
 *   - draw_scanline doubles pixels horizontally (320→640) and vertically
 *     (256→512, centred in 480 VGA lines).
 *
 * Task layout:
 *   Core 0  emulatorTask  — BBC Micro main loop (CPU + peripherals)
 *   Core 1  (FabGL ISR)   — VGA scanline DMA (runs autonomously)
 *   Core 1  audioTask     — SN76489 audio rendering
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

#include "fabgl.h"

#include "board.h"
#include "bbc_machine.h"
#include "sn76489.h"
#include "roms.h"

/* -----------------------------------------------------------------------
 * Back-buffer dimensions
 * ----------------------------------------------------------------------- */
#define BBC_BUF_W   320
#define BBC_BUF_H   256

/* -----------------------------------------------------------------------
 * BBC colour palette (index 0-7 → RGB888)
 * bit0=R, bit1=G, bit2=B (VideoULA convention)
 * ----------------------------------------------------------------------- */
static const fabgl::RGB888 s_bbc_palette[8] = {
    {   0,   0,   0 },   /* 0 Black   */
    { 255,   0,   0 },   /* 1 Red     */
    {   0, 255,   0 },   /* 2 Green   */
    { 255, 255,   0 },   /* 3 Yellow  */
    {   0,   0, 255 },   /* 4 Blue    */
    { 255,   0, 255 },   /* 5 Magenta */
    {   0, 255, 255 },   /* 6 Cyan    */
    { 255, 255, 255 },   /* 7 White   */
};

/* -----------------------------------------------------------------------
 * Global objects
 * ----------------------------------------------------------------------- */
static fabgl::VGADirectController s_vga;

/* Back-buffer in PSRAM: BBC_BUF_H rows × BBC_BUF_W cols, 1 byte/pixel.
 * Written by emulatorTask (Core 0), read by FabGL scanline ISR (Core 1). */
static uint8_t *s_backbuf = nullptr;

/* Pre-computed 8bpp VGA signal bytes for each BBC colour index. */
static uint8_t s_sig[8];

/* BBC machine state — allocated in PSRAM */
static bbc_machine_t *machine = nullptr;

/* -----------------------------------------------------------------------
 * VGADirectController draw-scanline callback (IRAM, Core 1 ISR context)
 *
 * VGA 640×480@60Hz → 480 scanlines.
 * BBC active area: 256 rows → 512 VGA lines (double-scan).
 * Vertical centering: top offset = (480 - 512) / 2 = -16
 *   → BBC row 0 maps to VGA scanLine 0 (top 16 BBC rows slightly clipped).
 *   Simple mapping: bbc_row = scanLine / 2; blank if out of [0, BBC_BUF_H).
 * ----------------------------------------------------------------------- */
static void IRAM_ATTR draw_scanline(void * /*arg*/, uint8_t *dest, int scanLine)
{
    int bbc_row = scanLine >> 1;
    if (bbc_row >= BBC_BUF_H || !s_backbuf) {
        memset(dest, s_sig[0], BBC_BUF_W * 2);
        return;
    }

    const uint8_t *src = s_backbuf + bbc_row * BBC_BUF_W;
    for (int x = 0; x < BBC_BUF_W; x++) {
        uint8_t v = s_sig[src[x] & 7];
        dest[x * 2    ] = v;
        dest[x * 2 + 1] = v;
    }
}

/* -----------------------------------------------------------------------
 * frame_cb — called by bbc_video after each full frame is "rendered".
 * In this build bbc_video writes into the INDEX8 framebuffer directly;
 * the draw_scanline ISR reads it on every VGA scanline.
 * Nothing needed here — the ISR reads s_backbuf at its own pace.
 * ----------------------------------------------------------------------- */
static void on_frame_ready(void * /*ctx*/)
{
    /* No explicit flip needed — single-buffer, ISR reads latest data */
}

/* -----------------------------------------------------------------------
 * Audio task — renders SN76489 samples and pushes them to the DAC.
 * Runs on Core 1 so audio is independent of emulator timing.
 * ----------------------------------------------------------------------- */
static void audioTask(void *arg)
{
    (void)arg;

    if (sn76489_audio_init() != ESP_OK) {
        printf("[audio] audio init failed\n");
        vTaskDelete(NULL);
        return;
    }

    printf("[audio] audio started at %lu Hz\n",
           (unsigned long)sn76489_audio_sample_rate());

    while (true) {
        if (machine)
            sn76489_audio_push(&machine->psg);
        else
            vTaskDelay(1);
    }
}

/* -----------------------------------------------------------------------
 * Emulator task — BBC Micro at 2 MHz on Core 0.
 * ----------------------------------------------------------------------- */
static void emulatorTask(void *arg)
{
    (void)arg;

    /* Configure video: INDEX8 framebuffer in PSRAM back-buffer */
    bbc_video_output_t fb_out = {};
    fb_out.format      = BBC_FB_FORMAT_INDEX8;
    fb_out.width       = BBC_BUF_W;
    fb_out.height      = BBC_BUF_H;
    fb_out.framebuffer = s_backbuf;
    fb_out.fb_stride   = BBC_BUF_W;

    bbc_machine_set_video_output(machine, &fb_out);
    bbc_machine_set_frame_callback(machine, on_frame_ready, nullptr);

    bbc_machine_reset(machine);
    printf("[emu] BBC Micro reset, running...\n");

    /*
     * 2 MHz 6502 → 2000 cycles per 1 ms FreeRTOS tick.
     */
    int cycles_budget = 0;
    while (true) {
        cycles_budget += 2000;
        while (cycles_budget > 0) {
            int c = bbc_machine_step(machine);
            cycles_budget -= c;
        }
        bbc_video_render_frame(&machine->video);
        vTaskDelay(1);
    }
}

/* -----------------------------------------------------------------------
 * app_main
 * ----------------------------------------------------------------------- */
extern "C" void app_main(void)
{
    printf("BBC Micro Emulator for ESP32 (PSRAM build)\n");
    printf("[mem] free PSRAM at start: %lu bytes\n",
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* --- VGA init ------------------------------------------------------- */
    s_vga.begin(BOARD_VGA_R1,    BOARD_VGA_R0,
                BOARD_VGA_G1,    BOARD_VGA_G0,
                BOARD_VGA_B1,    BOARD_VGA_B0,
                BOARD_VGA_HSYNC, BOARD_VGA_VSYNC);

    s_vga.setResolution(VGA_640x480_60Hz);
    s_vga.setDrawScanlineCallback(draw_scanline, nullptr);

    for (int i = 0; i < 8; i++)
        s_sig[i] = s_vga.createRawPixel(s_bbc_palette[i]);

    printf("[vga] VGADirectController running\n");

    /* --- Back-buffer in PSRAM ------------------------------------------- */
    s_backbuf = (uint8_t *)heap_caps_malloc(
        BBC_BUF_H * BBC_BUF_W, MALLOC_CAP_SPIRAM);
    if (!s_backbuf) {
        printf("[main] back-buffer alloc failed — need PSRAM\n");
        return;
    }
    memset(s_backbuf, 0, BBC_BUF_H * BBC_BUF_W);

    /* --- Machine alloc in PSRAM ----------------------------------------- */
    machine = (bbc_machine_t *)heap_caps_malloc(
        sizeof(bbc_machine_t), MALLOC_CAP_SPIRAM);
    if (!machine) {
        printf("[main] machine alloc failed — need PSRAM\n");
        return;
    }

    uint32_t os_size    = (uint32_t)(os12_rom_end   - os12_rom);
    uint32_t basic_size = (uint32_t)(basic2_rom_end - basic2_rom);
    printf("[main] OS ROM: %lu B  BASIC ROM: %lu B\n",
           (unsigned long)os_size, (unsigned long)basic_size);

    bbc_machine_init(machine,
                     os12_rom,   os_size,
                     basic2_rom, basic_size);

    printf("[mem] free PSRAM after init: %lu bytes\n",
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    printf("[main] starting tasks\n");
    xTaskCreatePinnedToCore(audioTask,    "audio", 4096, NULL, 10, NULL, 1);
    xTaskCreatePinnedToCore(emulatorTask, "emu",   8192, NULL,  5, NULL, 0);
}
