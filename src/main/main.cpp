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
 * Back-buffer dimensions
 * ----------------------------------------------------------------------- */
/* Back-buffer matches VGA width exactly (640 pixels).
 * bbc_video renders directly at 640×256 — no horizontal doubling needed
 * in draw_scanline.  Teletext (Mode 7) needs 40 cols × 12px = 480 ≤ 640;
 * bitmap modes need up to 640px at 2MHz clock — both fit. */
#define BBC_BUF_W   640
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

/* Double-buffered back-buffers in PSRAM (BBC_BUF_H × BBC_BUF_W, 1B/px).
 *
 * s_draw_idx: index of the buffer currently being READ by draw_scanline ISR.
 *   The emulator always writes to s_buf[1 - s_draw_idx].
 *   After a complete frame is rendered, the emulator atomically flips
 *   s_draw_idx so the ISR picks up the new frame on the next VGA field.
 *   No mutex needed: the flip is a single 32-bit store (atomic on Xtensa).
 */
static uint8_t *s_buf[2]       = { nullptr, nullptr };
static volatile uint32_t s_draw_idx = 0;   /* ISR reads s_buf[s_draw_idx] */

/* Pre-computed 8bpp VGA signal bytes for each BBC colour index. */
static uint8_t s_sig[8];

/* BBC machine state — allocated in PSRAM */
static bbc_machine_t *machine = nullptr;

/* -----------------------------------------------------------------------
 * VGADirectController draw-scanline callback (IRAM, Core 1 ISR context)
 * ----------------------------------------------------------------------- */
/* VGA is 640×480.  BBC back-buffer is 640×256 (1:1 horizontally).
 * Vertical mapping: 256 BBC rows → 480 VGA lines.
 *   BBC row = scanLine * 256 / 480  (nearest-neighbour scaling)
 * No horizontal doubling needed — bbc_video renders at full 640px width.
 *
 * Mode 7 (Teletext): render_teletext_frame() centres content horizontally.
 *   40 cols × 12 px = 480 px content, x_offset = (640-480)/2 = 80 px.
 *   x_offset must be a multiple of 4 so the I2S swizzle lands on aligned
 *   word boundaries — 80 is divisible by 4, so no sub-word pixel reorder.
 *
 * I2S LCD mode serialises 32-bit words as [byte2, byte3, byte0, byte1].
 * Writing dest[x ^ 2] compensates: the hardware re-orders back to [0,1,2,3]
 * at the output pins.  x_offset being a multiple of 4 keeps groups aligned.
 */
static void IRAM_ATTR draw_scanline(void * /*arg*/, uint8_t *dest, int scanLine)
{
    /* Read the buffer that the emulator last completed (not the one being
     * written now).  s_draw_idx is flipped by on_frame_ready() after each
     * complete frame — a single 32-bit store, atomic on Xtensa. */
    const uint8_t *fb = s_buf[s_draw_idx];
    if (!fb) {
        memset(dest, s_sig[0], 640);
        return;
    }

    /* Scale 480 VGA lines → 256 BBC rows (nearest-neighbour) */
    int bbc_row = scanLine * BBC_BUF_H / 480;
    if (bbc_row >= BBC_BUF_H) bbc_row = BBC_BUF_H - 1;

    const uint8_t *src = fb + bbc_row * BBC_BUF_W;
    for (int x = 0; x < 640; x++) {
        dest[x ^ 2] = s_sig[src[x] & 7];
    }
}

/* Called by bbc_video after each complete frame is rendered into the
 * write buffer.  Flip s_draw_idx so the ISR picks up the new frame.
 * This is a single 32-bit store — atomic on Xtensa without a mutex. */
static void on_frame_ready(void * /*ctx*/)
{
    s_draw_idx ^= 1u;
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

    /* Emulator always writes to the buffer NOT being read by the ISR.
     * s_draw_idx starts at 0 → ISR reads s_buf[0] → we write s_buf[1].
     * on_frame_ready() flips s_draw_idx after each complete frame. */
    bbc_video_output_t fb_out = {};
    fb_out.format      = BBC_FB_FORMAT_INDEX8;
    fb_out.width       = BBC_BUF_W;
    fb_out.height      = BBC_BUF_H;
    fb_out.framebuffer = s_buf[1];   /* write buffer = 1 - s_draw_idx(=0) */
    fb_out.fb_stride   = BBC_BUF_W;

    bbc_machine_set_video_output(machine, &fb_out);
    bbc_machine_set_frame_callback(machine, on_frame_ready, nullptr);

    bbc_machine_reset(machine);
    printf("[emu] BBC Micro reset, running...\n");

    int cycles_budget = 0;
    while (true) {
        cycles_budget += 2000;
        while (cycles_budget > 0) {
            int c = bbc_machine_step(machine);
            cycles_budget -= c;
        }
        /* Render into the write buffer (1 - s_draw_idx), then flip. */
        uint32_t write_idx = 1u - s_draw_idx;
        fb_out.framebuffer = s_buf[write_idx];
        bbc_machine_set_video_output(machine, &fb_out);
        bbc_video_render_frame(&machine->video);
        /* on_frame_ready() flips s_draw_idx after render completes */
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

    /* --- Double back-buffers in PSRAM ------------------------------------ */
    for (int i = 0; i < 2; i++) {
        s_buf[i] = (uint8_t *)heap_caps_malloc(
            BBC_BUF_H * BBC_BUF_W, MALLOC_CAP_SPIRAM);
        if (!s_buf[i]) {
            printf("[main] back-buffer[%d] alloc failed — need PSRAM\n", i);
            return;
        }
        memset(s_buf[i], 0, BBC_BUF_H * BBC_BUF_W);
    }
    /* ISR starts reading s_buf[0]; emulator starts writing s_buf[1]. */
    s_draw_idx = 0;

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
