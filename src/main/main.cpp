/*
 * main.cpp — ESP32 BBC Micro emulator entry point
 *
 * Hardware target: Olimex ESP32-SBC-FabGL (ESP32-WROVER-E, 4 MB Flash + 8 MB PSRAM)
 * Pin assignments: see board.h
 *
 * Display: FabGL VGADirectController (64-colour, 8 GPIOs)
 *   - No persistent framebuffer — pixels are rendered directly into FabGL's
 *     DMA scan-line buffer on demand by the draw_scanline ISR (Core 1).
 *   - draw_scanline calls bbc_video_render_row() for each VGA scanline,
 *     mapping VGA line 0-479 → BBC row 0-255 (nearest-neighbour, 256→480).
 *   - A 640-byte DRAM row buffer (s_row_buf) is the only render scratch space.
 *   - Saves ~320 KB PSRAM vs. the previous double-framebuffer approach.
 *
 * Keyboard: FabGL PS2Controller → BBC Micro keyboard matrix
 *   - PS/2 on GPIO 33 (CLK) / 32 (DATA) per board.h
 *   - VirtualKey → (row, col) mapping for BBC Model B layout
 *   - keyboardTask polls getNextVirtualKey() and calls bbc_machine_key_event()
 *
 * SD card: SPI, FAT filesystem via esp_vfs_fat
 *   - Pines de board.h (MOSI=13, MISO=35, CLK=14, CS=2) — VERIFY on hardware
 *   - Mounts /sdcard; loads first .ssd or .dsd found into drive 0
 *   - SSD format: 80 tracks × 10 sectors × 256 bytes, single-sided
 *   - DSD format: 80 tracks × 10 sectors × 256 bytes × 2 sides (interleaved)
 *
 * Task layout:
 *   Core 0  emulatorTask  — BBC Micro main loop (CPU + peripherals)
 *   Core 1  (FabGL ISR)   — VGA scanline DMA (runs autonomously)
 *   Core 1  audioTask     — SN76489 audio rendering
 *   Core 1  keyboardTask  — PS/2 → BBC matrix events
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "esp_task_wdt.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#include "fabgl.h"

#include "board.h"
#include "bbc_machine.h"
#include "sn76489.h"
#include "roms.h"

static const char *TAG_MAIN = "main";
static const char *TAG_SD   = "sd";

/* -----------------------------------------------------------------------
 * VGA / BBC output width
 * ----------------------------------------------------------------------- */
/* BBC content renders at 640 pixels wide (1:1 with VGA 640×480).
 * Teletext (Mode 7): 40 cols × 12 px = 480 px, centred with 80 px border.
 * Bitmap modes: up to 640 px at 2 MHz clock — fits exactly. */
#define BBC_BUF_W   640     /* pixels per VGA line / BBC row */

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
 * ----------------------------------------------------------------------- */
struct BbcKeyPos { uint8_t row; uint8_t col; };

static BbcKeyPos vk_to_bbc(fabgl::VirtualKey vk)
{
    using namespace fabgl;
    switch (vk) {
    /* Row 0 */
    case VK_LSHIFT:     case VK_RSHIFT:     return {0, 0};
    case VK_q:          case VK_Q:          return {0, 1};
    case VK_F10:                            return {0, 2};
    case VK_1:                              return {0, 3};
    case VK_CAPSLOCK:                       return {0, 4};
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
    case VK_AT:                             return {7, 4};
    case VK_COLON:      case VK_SEMICOLON:  return {7, 5};
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
    default:                                return {0xFF, 0xFF};
    }
}

/* -----------------------------------------------------------------------
 * SD card / disk image context
 *
 * BBC DFS disk formats:
 *   SSD: single-sided, 80 tracks × 10 sectors × 256 B
 *        offset(track, sector) = (track * 10 + sector) * 256
 *   DSD: double-sided, sides interleaved by track:
 *        offset(track, side, sector) = ((track * 2 + side) * 10 + sector) * 256
 * ----------------------------------------------------------------------- */
#define BBC_SECTORS_PER_TRACK  10
#define BBC_SECTOR_SIZE        256
#define BBC_TRACKS             80

typedef struct {
    FILE   *fp;          /* open file handle; NULL = no disk */
    bool    is_dsd;      /* true = double-sided (.dsd), false = single (.ssd) */
    bool    read_only;   /* write-protect flag */
} bbc_disk_ctx_t;

static bbc_disk_ctx_t s_disk[2];   /* drive 0 and drive 1 */

static int disk_read_sector(void *user_ctx,
                             uint8_t drive, uint8_t track, uint8_t sector,
                             uint8_t side, uint8_t /*density*/,
                             uint8_t *buf, uint16_t *len)
{
    bbc_disk_ctx_t *ctx = (bbc_disk_ctx_t *)user_ctx;
    if (!ctx || !ctx->fp) return -1;
    if (track  >= BBC_TRACKS)             return -1;
    if (sector >= BBC_SECTORS_PER_TRACK)  return -1;
    if (!ctx->is_dsd && side != 0)        return -1;

    long offset;
    if (ctx->is_dsd) {
        offset = (long)((track * 2 + side) * BBC_SECTORS_PER_TRACK + sector)
                 * BBC_SECTOR_SIZE;
    } else {
        offset = (long)(track * BBC_SECTORS_PER_TRACK + sector)
                 * BBC_SECTOR_SIZE;
    }

    if (fseek(ctx->fp, offset, SEEK_SET) != 0) return -1;
    size_t n = fread(buf, 1, BBC_SECTOR_SIZE, ctx->fp);
    if (n != BBC_SECTOR_SIZE) return -1;

    *len = BBC_SECTOR_SIZE;
    return 0;
}

static int disk_write_sector(void *user_ctx,
                              uint8_t drive, uint8_t track, uint8_t sector,
                              uint8_t side, uint8_t /*density*/, bool /*deleted*/,
                              const uint8_t *buf, uint16_t len)
{
    bbc_disk_ctx_t *ctx = (bbc_disk_ctx_t *)user_ctx;
    if (!ctx || !ctx->fp)   return -1;
    if (ctx->read_only)     return -1;
    if (track  >= BBC_TRACKS)             return -1;
    if (sector >= BBC_SECTORS_PER_TRACK)  return -1;
    if (!ctx->is_dsd && side != 0)        return -1;
    if (len != BBC_SECTOR_SIZE)           return -1;

    long offset;
    if (ctx->is_dsd) {
        offset = (long)((track * 2 + side) * BBC_SECTORS_PER_TRACK + sector)
                 * BBC_SECTOR_SIZE;
    } else {
        offset = (long)(track * BBC_SECTORS_PER_TRACK + sector)
                 * BBC_SECTOR_SIZE;
    }

    if (fseek(ctx->fp, offset, SEEK_SET) != 0) return -1;
    size_t n = fwrite(buf, 1, BBC_SECTOR_SIZE, ctx->fp);
    if (n != BBC_SECTOR_SIZE) return -1;
    fflush(ctx->fp);
    return 0;
}

static void disk_seek(void * /*user_ctx*/, uint8_t /*drive*/, uint8_t /*track*/)
{
    /* No physical head movement needed for file-backed images */
}

/*
 * Mount SD card via SPI and try to open the first .ssd or .dsd file found.
 * Returns true if a disk image was found and mounted on drive 0.
 */
static bool sd_init_and_mount_disk(bbc_machine_t *m)
{
    /* --- Mount SD card -------------------------------------------------- */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs   = BOARD_SD_CS;
    slot_config.host_id   = (spi_host_device_t)host.slot;

    /* SPI bus config */
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num   = BOARD_SD_MOSI;
    bus_cfg.miso_io_num   = BOARD_SD_MISO;
    bus_cfg.sclk_io_num   = BOARD_SD_CLK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;

    esp_err_t err = spi_bus_initialize((spi_host_device_t)host.slot,
                                        &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_SD, "SPI bus init failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files              = 4;
    mount_cfg.allocation_unit_size  = 0;

    sdmmc_card_t *card = nullptr;
    err = esp_vfs_fat_sdspi_mount(BOARD_SD_MOUNT, &host, &slot_config,
                                   &mount_cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_SD, "SD mount failed: %s — no disk available",
                 esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG_SD, "SD mounted at %s", BOARD_SD_MOUNT);
    sdmmc_card_print_info(stdout, card);

    /* --- Find first .ssd or .dsd ---------------------------------------- */
    DIR *dir = opendir(BOARD_SD_MOUNT);
    if (!dir) {
        ESP_LOGW(TAG_SD, "Cannot open %s", BOARD_SD_MOUNT);
        return false;
    }

    char img_path[300] = {};
    bool img_is_dsd = false;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type != DT_REG) continue;
        const char *name = ent->d_name;
        size_t nlen = strlen(name);
        if (nlen < 4) continue;
        const char *ext = name + nlen - 4;
        if (strcasecmp(ext, ".ssd") == 0 || strcasecmp(ext, ".dsd") == 0) {
            snprintf(img_path, sizeof(img_path), "%s/%s",
                     BOARD_SD_MOUNT, name);
            img_is_dsd = (strcasecmp(ext, ".dsd") == 0);
            break;
        }
    }
    closedir(dir);

    if (img_path[0] == '\0') {
        ESP_LOGW(TAG_SD, "No .ssd or .dsd found on SD card");
        return false;
    }

    /* --- Open image and mount on drive 0 -------------------------------- */
    s_disk[0].fp        = fopen(img_path, "r+b");
    s_disk[0].is_dsd    = img_is_dsd;
    s_disk[0].read_only = false;

    if (!s_disk[0].fp) {
        /* Try read-only */
        s_disk[0].fp        = fopen(img_path, "rb");
        s_disk[0].read_only = true;
    }

    if (!s_disk[0].fp) {
        ESP_LOGE(TAG_SD, "Cannot open %s", img_path);
        return false;
    }

    ESP_LOGI(TAG_SD, "Mounting %s as drive 0 (%s, %s)",
             img_path,
             img_is_dsd ? "DSD" : "SSD",
             s_disk[0].read_only ? "read-only" : "read-write");

    bbc_machine_mount_disk(m, 0,
                           disk_read_sector,
                           disk_write_sector,
                           disk_seek,
                           &s_disk[0]);
    return true;
}

/* -----------------------------------------------------------------------
 * Global objects
 * ----------------------------------------------------------------------- */
static fabgl::VGADirectController s_vga;
static fabgl::PS2Controller       s_ps2;

/* Per-scanline render scratch buffer in DRAM (not PSRAM — ISR must access
 * it at wire speed without PSRAM latency).  640 bytes = one VGA line. */
static uint8_t s_row_buf[BBC_BUF_W];

/* Per-frame video state snapshot taken at VGA VSYNC (scanLine == 0).
 *
 * The ISR (Core 1) and the emulator (Core 0) share bbc_machine_t in PSRAM.
 * Without a snapshot, CRTC/ULA registers written by Core 0 mid-frame would
 * cause pixel tearing or mode-switch glitches within a single VGA field.
 *
 * At the start of each VGA frame (scanLine == 0) the ISR copies the relevant
 * video sub-state from machine->video into s_video_snap.  All 480 scanline
 * calls within that frame then render from the snapshot — a consistent,
 * immutable view of the BBC video state for the duration of one VGA field.
 *
 * system_ram is NOT copied (up to 32 KB); the pointer is kept.  RAM writes
 * from Core 0 may still cause pixel-level tearing within a frame, but this
 * is visually acceptable and avoids a costly memcpy in the ISR.
 *
 * The snapshot contains: mc6845_t registers, bbc_video_ula_t state, and the
 * saa5050_t state (DH propagation table).  Total: ~sizeof(bbc_video_t) minus
 * the callbacks and frame counter, dominated by the SAA5050 char ROM pointer.
 *
 * Layout in DRAM: two ping-pong slots so Core 0 can write the next snapshot
 * while Core 1 finishes reading the current one.  s_snap_idx selects the
 * active slot for the ISR; Core 0 writes to 1 - s_snap_idx then flips.
 *
 * For simplicity we use a single slot with a volatile flag.  Worst case:
 * Core 0 writes the snapshot just as Core 1 reads it for scanLine 0 of the
 * next frame.  This race affects at most one frame per second at BBC speed.
 */
static bbc_video_t s_video_snap;           /* snapshot read by ISR          */
static volatile bool s_snap_ready = false; /* true once first snap is taken */

/* Pre-computed 8bpp VGA signal bytes for each BBC colour index. */
static uint8_t s_sig[8];

/* BBC machine state — allocated in PSRAM */
static bbc_machine_t *machine = nullptr;

/* -----------------------------------------------------------------------
 * VGADirectController draw-scanline callback (IRAM, Core 1 ISR context)
 * ----------------------------------------------------------------------- */
/* VGA is 640×480.  BBC native output is 640×256 (1:1 horizontally).
 * Vertical mapping: 256 BBC rows → 480 VGA lines (nearest-neighbour ×1.875).
 *
 * At scanLine == 0 (VSYNC / start of VGA frame): snapshot CRTC + ULA + SAA5050
 * state from machine->video into s_video_snap.  All scanlines in this frame
 * render from the snapshot for a consistent, tear-free image within one field.
 *
 * system_ram is shared (not snapshotted) to avoid a 32 KB ISR memcpy; RAM
 * writes by Core 0 may cause sub-frame pixel tearing but not corruption.
 *
 * I2S LCD serialises 32-bit words as [byte2, byte3, byte0, byte1].
 * Writing dest[x ^ 2] compensates so the output pin order is [0,1,2,3].
 * x_offset=80 (teletext border) is a multiple of 4 — word-aligned.
 */
static void IRAM_ATTR draw_scanline(void * /*arg*/, uint8_t *dest, int scanLine)
{
    if (!machine) {
        memset(dest, s_sig[0], BBC_BUF_W);
        return;
    }

    /* At the start of each VGA frame, snapshot video registers from PSRAM.
     * This is the only point where we touch machine->video (in PSRAM);
     * all subsequent scanline calls use s_video_snap (DRAM). */
    if (scanLine == 0) {
        /* Copy CRTC, ULA, SAA5050 state; keep system_ram pointer from snap. */
        s_video_snap            = machine->video;
        /* Clear mutable render-only fields that the snapshot must not carry */
        s_video_snap.frame_cb   = nullptr;
        s_video_snap.vsync_cb   = nullptr;
        s_video_snap.frame_ctx  = nullptr;
        s_video_snap.vsync_ctx  = nullptr;
        s_snap_ready            = true;
    }

    if (!s_snap_ready) {
        memset(dest, s_sig[0], BBC_BUF_W);
        return;
    }

    bbc_video_render_row(&s_video_snap, scanLine, 480, s_row_buf, BBC_BUF_W);

    for (int x = 0; x < BBC_BUF_W; x++)
        dest[x ^ 2] = s_sig[s_row_buf[x] & 7];
}

/* -----------------------------------------------------------------------
 * Keyboard task
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
        if (kb->getNextVirtualKey(&item, 10)) {
            if (!machine) continue;
            BbcKeyPos pos = vk_to_bbc(item.vk);
            if (pos.row == 0xFF) continue;
            bbc_machine_key_event(machine, pos.row, pos.col, item.down);
        }
    }
}

/* -----------------------------------------------------------------------
 * Audio task
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

    /* Each buffer is 256 samples @ 22050 Hz ≈ 11.6 ms.  Delay after each
     * push so IDLE1 always gets CPU time regardless of whether
     * xQueueReceive blocked or returned immediately. */
    while (true) {
        if (machine)
            sn76489_audio_push(&machine->psg);
        vTaskDelay(pdMS_TO_TICKS(11));
    }
}

/* -----------------------------------------------------------------------
 * Emulator task
 * ----------------------------------------------------------------------- */
static void emulatorTask(void *arg)
{
    (void)arg;

    /* No framebuffer needed — draw_scanline renders directly via
     * bbc_video_render_row().  The emulator just runs BBC CPU cycles and
     * updates machine state; the VGA ISR (Core 1) reads that state on demand.
     *
     * Timing: BBC 6502 runs at ~2 MHz; FreeRTOS tick at 1000 Hz (1 ms).
     * Target: 2 000 000 / 1000 = 2000 cycles per tick.
     * bbc_machine_step() returns the cycle count of each instruction (2-7).
     * We run a budget of 2000 cycles per vTaskDelay(1) tick. */
    bbc_machine_reset(machine);
    printf("[emu] BBC Micro reset, running...\n");

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

    s_vga.setDrawScanlineCallback(draw_scanline, nullptr);

    /* setResolution() → startGPIOStream() → rtc_clk_apll_coeff_set() contains
     * a bare spin-wait on APLL hardware calibration (esp_rom_delay_us in ROM)
     * that holds CPU 0 long enough to starve IDLE0 and trigger the TWDT.
     * Temporarily raise the timeout to 30 s for this one-time init call, then
     * restore the configured value immediately after. */
    {
        esp_task_wdt_config_t twdt_long = {
            .timeout_ms     = 30000,
            .idle_core_mask = (1 << 0) | (1 << 1),
            .trigger_panic  = false,
        };
        esp_task_wdt_reconfigure(&twdt_long);
        s_vga.setResolution(VGA_640x480_60Hz);
        esp_task_wdt_config_t twdt_normal = {
            .timeout_ms     = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000,
            .idle_core_mask = (1 << 0) | (1 << 1),
            .trigger_panic  = false,
        };
        esp_task_wdt_reconfigure(&twdt_normal);
    }

    /* createRawPixel() uses m_HVSync which is set inside setResolution().
     * Must be called AFTER setResolution() or all pixels get sync=0. */
    for (int i = 0; i < 8; i++)
        s_sig[i] = s_vga.createRawPixel(s_bbc_palette[i]);

    printf("[vga] VGADirectController running\n");

    /* --- PS/2 keyboard init --------------------------------------------- */
    s_ps2.begin(BOARD_PS2_KBD_CLK, BOARD_PS2_KBD_DATA);
    printf("[kbd] PS/2 controller on CLK=%d DATA=%d\n",
           (int)BOARD_PS2_KBD_CLK, (int)BOARD_PS2_KBD_DATA);

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

    /* --- SD card + disk image ------------------------------------------- */
    memset(s_disk, 0, sizeof(s_disk));
    if (sd_init_and_mount_disk(machine)) {
        printf("[sd] disk image mounted on drive 0\n");
    } else {
        printf("[sd] no disk — running without floppy\n");
    }

    printf("[mem] free PSRAM after init: %lu bytes\n",
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    printf("[main] starting tasks\n");
    /* Priority 2: above IDLE (0) and timer daemon (1) but below everything
     * else.  Audio latency is guaranteed by DMA — the task just needs to
     * refill buffers before the DAC starves, not run at high priority. */
    xTaskCreatePinnedToCore(audioTask,    "audio", 4096, NULL,  2, NULL, 1);
    xTaskCreatePinnedToCore(keyboardTask, "kbd",   2048, NULL,  8, NULL, 1);
    xTaskCreatePinnedToCore(emulatorTask, "emu",   8192, NULL,  5, NULL, 0);
}
