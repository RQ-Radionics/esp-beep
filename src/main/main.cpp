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
 * Keyboard: FabGL PS2Controller → BBC Micro keyboard matrix
 *   - PS/2 on GPIO 33 (CLK) / 32 (DATA) per board.h
 *   - VirtualKey → (row, col) mapping for BBC Model B layout
 *   - keyboardTask polls getNextVirtualKey() and calls bbc_machine_key_event()
 *
 * Task layout:
 *   Core 0  emulatorTask  — BBC Micro main loop (CPU + peripherals)
 *   Core 1  (FabGL ISR)   — VGA scanline DMA (runs autonomously)
 *   Core 1  audioTask     — SN76489 audio rendering
 *   Core 1  keyboardTask  — PS/2 → BBC matrix events
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
 * BBC Micro Model B keyboard matrix mapping
 *
 * The BBC keyboard is a 10-row × 8-column matrix.
 * Columns 0-7 are strobed; rows 0-9 are sensed.
 *
 * Physical layout (from Acorn BBC Micro Advanced User Guide):
 *
 *  row\col  0       1       2       3       4       5       6       7
 *  0        SHIFT   Q       F0      1       CAPS    SHIFTLK TAB     ESCAPE
 *  1        CTRL    3       W       2       A       S       Z       (none)
 *  2        (none)  4       E       R       D       F       X       C
 *  3        (none)  5       T       6       G       H       V       B
 *  4        (none)  F4      7       8       Y       J       N       SPACE
 *  5        (none)  F5      I       O       U       K       M       COMMA
 *  6        (none)  F6      9       0       P       L       (none)  PERIOD
 *  7        (none)  F7      MINUS   EQUALS  AT      COLON   SLASH   (none)
 *  8        F1      F2      F3      BREAK   (none)  UP      (none)  DELETE
 *  9        (none)  (none)  (none)  COPY    (none)  RIGHT   RETURN  (none)
 *
 * (COPY = end-of-line / copy key on BBC; we map it to END)
 * (AT = '@'; COLON = ':'; BREAK = F12 on PC)
 *
 * Encoding: BBC_KEY(row, col) packed as (row<<4)|col — 0xFF = no mapping.
 * ----------------------------------------------------------------------- */
#define BBC_KEY(r, c)  (uint8_t)(((r) << 4) | (c))
#define BBC_NONE       0xFF

/* Lookup table: indexed by VirtualKey enum value.
 * VK_NONE=0 is not in the table; we check for VK_NONE explicitly.
 * Size must cover all VK_ values we care about. */

struct BbcKeyPos { uint8_t row; uint8_t col; };

/* Returns {0xFF,0xFF} for no mapping */
static BbcKeyPos vk_to_bbc(fabgl::VirtualKey vk)
{
    using namespace fabgl;
    switch (vk) {
    /* Row 0 */
    case VK_LSHIFT:     case VK_RSHIFT:     return {0, 0};
    case VK_q:          case VK_Q:          return {0, 1};
    case VK_F10:                            return {0, 2}; /* F0 on BBC */
    case VK_1:                              return {0, 3};
    case VK_CAPSLOCK:                       return {0, 4};
    /* SHIFTLK (shift-lock) — no direct PS/2 equivalent, skip */
    case VK_TAB:                            return {0, 6};
    case VK_ESCAPE:                         return {0, 7};

    /* Row 1 */
    case VK_LCTRL:      case VK_RCTRL:      return {1, 0};
    case VK_3:                              return {1, 1};
    case VK_w:          case VK_W:          return {1, 2};
    case VK_2:                              return {1, 3};
    case VK_a:          case VK_A:          return {1, 4};
    case VK_s:          case VK_S:          return {1, 5};
    case VK_z:          case VK_Z:          return {1, 6};

    /* Row 2 */
    case VK_4:                              return {2, 1};
    case VK_e:          case VK_E:          return {2, 2};
    case VK_r:          case VK_R:          return {2, 3};
    case VK_d:          case VK_D:          return {2, 4};
    case VK_f:          case VK_F:          return {2, 5};
    case VK_x:          case VK_X:          return {2, 6};
    case VK_c:          case VK_C:          return {2, 7};

    /* Row 3 */
    case VK_5:                              return {3, 1};
    case VK_t:          case VK_T:          return {3, 2};
    case VK_6:                              return {3, 3};
    case VK_g:          case VK_G:          return {3, 4};
    case VK_h:          case VK_H:          return {3, 5};
    case VK_v:          case VK_V:          return {3, 6};
    case VK_b:          case VK_B:          return {3, 7};

    /* Row 4 */
    case VK_F4:                             return {4, 1};
    case VK_7:                              return {4, 2};
    case VK_8:                              return {4, 3};
    case VK_y:          case VK_Y:          return {4, 4};
    case VK_j:          case VK_J:          return {4, 5};
    case VK_n:          case VK_N:          return {4, 6};
    case VK_SPACE:                          return {4, 7};

    /* Row 5 */
    case VK_F5:                             return {5, 1};
    case VK_i:          case VK_I:          return {5, 2};
    case VK_o:          case VK_O:          return {5, 3};
    case VK_u:          case VK_U:          return {5, 4};
    case VK_k:          case VK_K:          return {5, 5};
    case VK_m:          case VK_M:          return {5, 6};
    case VK_COMMA:                          return {5, 7};

    /* Row 6 */
    case VK_F6:                             return {6, 1};
    case VK_9:                              return {6, 2};
    case VK_0:                              return {6, 3};
    case VK_p:          case VK_P:          return {6, 4};
    case VK_l:          case VK_L:          return {6, 5};
    case VK_PERIOD:                         return {6, 7};

    /* Row 7 */
    case VK_F7:                             return {7, 1};
    case VK_MINUS:                          return {7, 2};
    case VK_EQUALS:                         return {7, 3};
    case VK_AT:                             return {7, 4}; /* '@' = BBC @ key */
    case VK_COLON:      case VK_SEMICOLON:  return {7, 5}; /* BBC ':'/';' same key */
    case VK_SLASH:                          return {7, 6};

    /* Row 8 */
    case VK_F1:                             return {8, 0};
    case VK_F2:                             return {8, 1};
    case VK_F3:                             return {8, 2};
    case VK_F12:                            return {8, 3}; /* BREAK */
    case VK_UP:         case VK_KP_UP:      return {8, 5};
    case VK_DELETE:     case VK_BACKSPACE:  return {8, 7};

    /* Row 9 */
    case VK_END:        case VK_KP_END:     return {9, 3}; /* COPY */
    case VK_RIGHT:      case VK_KP_RIGHT:   return {9, 5};
    case VK_RETURN:     case VK_KP_ENTER:   return {9, 6};

    /* Unmapped */
    default:                                return {0xFF, 0xFF};
    }
}

/* -----------------------------------------------------------------------
 * Global objects
 * ----------------------------------------------------------------------- */
static fabgl::VGADirectController s_vga;
static fabgl::PS2Controller       s_ps2;

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
 * Simple mapping: bbc_row = scanLine / 2; blank if out of [0, BBC_BUF_H).
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
 * frame_cb — no explicit flip needed (single-buffer, ISR reads latest).
 * ----------------------------------------------------------------------- */
static void on_frame_ready(void * /*ctx*/)
{
}

/* -----------------------------------------------------------------------
 * Keyboard task — reads VirtualKeys from FabGL and feeds BBC matrix.
 * Runs on Core 1.
 * ----------------------------------------------------------------------- */
static void keyboardTask(void *arg)
{
    (void)arg;

    fabgl::Keyboard *kb = s_ps2.keyboard();
    if (!kb) {
        printf("[kbd] no keyboard object — task exiting\n");
        vTaskDelete(NULL);
        return;
    }

    printf("[kbd] PS/2 keyboard task started\n");

    while (true) {
        fabgl::VirtualKeyItem item;
        if (kb->getNextVirtualKey(&item, 10 /* ms timeout */)) {
            if (!machine) continue;
            BbcKeyPos pos = vk_to_bbc(item.vk);
            if (pos.row == 0xFF) continue;          /* unmapped key */
            bbc_machine_key_event(machine, pos.row, pos.col, item.down);
        }
    }
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

    /* --- PS/2 keyboard init --------------------------------------------- */
    /*
     * Use explicit GPIO form so we read from board.h.
     * Only keyboard on port 0; no mouse.
     */
    s_ps2.begin(BOARD_PS2_KBD_CLK, BOARD_PS2_KBD_DATA);
    printf("[kbd] PS/2 controller init on CLK=%d DATA=%d\n",
           (int)BOARD_PS2_KBD_CLK, (int)BOARD_PS2_KBD_DATA);

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
    xTaskCreatePinnedToCore(keyboardTask, "kbd",   2048, NULL,  8, NULL, 1);
    xTaskCreatePinnedToCore(emulatorTask, "emu",   8192, NULL,  5, NULL, 0);
}
