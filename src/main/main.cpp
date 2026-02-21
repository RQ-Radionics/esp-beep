/*
 * main.cpp — ESP32 BBC Micro emulator entry point
 *
 * Task layout:
 *   Core 0  emulatorTask  — BBC Micro main loop (CPU + peripherals)
 *   Core 1  audioTask     — SN76489 → I2S audio rendering
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

#include "bbc_machine.h"
#include "sn76489.h"
#include "roms.h"

/* -----------------------------------------------------------------------
 * Global machine state — allocated in PSRAM (bbc_machine_t embeds ~32 KB
 * of RAM plus all chip state).
 * ----------------------------------------------------------------------- */
static bbc_machine_t *machine = nullptr;

/* 640×256 RGB565 framebuffer in PSRAM */
static uint16_t *framebuffer = nullptr;

/* -----------------------------------------------------------------------
 * Frame-ready callback — called from bbc_video_render_frame() inside the
 * emulator loop.  Future: signal display / swap buffers.
 * ----------------------------------------------------------------------- */
static void on_frame_ready(void * /*ctx*/) {
    /* TODO: signal VGA display task */
}

/* -----------------------------------------------------------------------
 * Audio task — renders SN76489 samples and pushes them to I2S DMA.
 * Runs on Core 1 so audio is independent of emulator timing.
 * ----------------------------------------------------------------------- */
static void audioTask(void *arg) {
    (void)arg;

    if (sn76489_audio_init() != ESP_OK) {
        printf("[audio] I2S init failed\n");
        vTaskDelete(NULL);
        return;
    }

    printf("[audio] I2S started at %lu Hz\n",
           (unsigned long)sn76489_audio_sample_rate());

    while (true) {
        if (machine) {
            sn76489_audio_push(&machine->psg);
        } else {
            vTaskDelay(1);
        }
    }
}

/* -----------------------------------------------------------------------
 * Emulator task — BBC Micro at full speed on Core 0.
 * ----------------------------------------------------------------------- */
static void emulatorTask(void *arg) {
    (void)arg;

    /* Allocate framebuffer in PSRAM */
    framebuffer = (uint16_t *)heap_caps_malloc(
        640 * 256 * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!framebuffer) {
        printf("[emu] framebuffer alloc failed — need PSRAM\n");
        vTaskDelete(NULL);
        return;
    }

    /* Configure video output */
    bbc_video_output_t fb_out = {
        .format      = BBC_FB_FORMAT_RGB565,
        .width       = 640,
        .height      = 256,
        .framebuffer = framebuffer,
        .fb_stride   = 640 * 2,
    };
    bbc_machine_set_video_output(machine, &fb_out);
    bbc_machine_set_frame_callback(machine, on_frame_ready, nullptr);

    /* Reset all chips and start CPU */
    bbc_machine_reset(machine);
    printf("[emu] BBC Micro reset, running...\n");

    /*
     * Run 2 MHz worth of CPU cycles per 1 ms FreeRTOS tick.
     * BBC Model B: 2 MHz 6502 → 2000 cycles/ms.
     * We use a cycle budget to absorb variable instruction lengths.
     */
    int cycles_budget = 0;

    while (true) {
        cycles_budget += 2000;
        while (cycles_budget > 0) {
            int c = bbc_machine_step(machine);
            cycles_budget -= c;
        }
        vTaskDelay(1);
    }
}

/* -----------------------------------------------------------------------
 * app_main
 * ----------------------------------------------------------------------- */
extern "C" void app_main(void) {
    printf("BBC Micro Emulator for ESP32\n");

    /* Allocate machine struct in PSRAM */
    machine = (bbc_machine_t *)heap_caps_malloc(
        sizeof(bbc_machine_t), MALLOC_CAP_SPIRAM);
    if (!machine) {
        printf("[main] machine alloc failed — PSRAM required\n");
        return;
    }

    uint32_t os_size    = (uint32_t)(os12_rom_end   - os12_rom);
    uint32_t basic_size = (uint32_t)(basic2_rom_end - basic2_rom);
    printf("[main] OS ROM   : %lu bytes\n", (unsigned long)os_size);
    printf("[main] BASIC ROM: %lu bytes\n", (unsigned long)basic_size);

    bbc_machine_init(machine,
                     os12_rom,   os_size,
                     basic2_rom, basic_size);

    printf("[main] Machine initialised — starting tasks\n");

    xTaskCreatePinnedToCore(audioTask,    "audio", 4096, NULL, 10, NULL, 1);
    xTaskCreatePinnedToCore(emulatorTask, "emu",   8192, NULL,  5, NULL, 0);
}
