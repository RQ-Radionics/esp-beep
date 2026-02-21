/*
 * main_sdl.c — BBC Micro emulator, native macOS/Linux host frontend
 *
 * Replaces src/main/main.cpp (ESP32 / FabGL / FreeRTOS) with:
 *   - SDL2 window 640×480, nearest-neighbour scaling
 *   - SDL2 audio callback → sn76489_render()
 *   - SDL2 keyboard → BBC Micro matrix via bbc_machine_key_event()
 *   - Disk images (.ssd / .dsd) loaded from argv[1]
 *
 * Build: make  (see Makefile)
 * Run:   ./bbc-beep [disk.ssd|disk.dsd]
 *
 * BBC colour palette (index 0-7): bit0=R bit1=G bit2=B
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>

#include <SDL.h>

#include "bbc_machine.h"
#include "bbc_video.h"
#include "sn76489.h"

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */
#define WIN_W        640
#define WIN_H        480
#define AUDIO_RATE   22050
#define AUDIO_FRAMES 512          /* SDL audio buffer size in samples        */
#define BBC_CPU_HZ   2000000      /* BBC 6502 clock: 2 MHz                   */
#define FPS_TARGET   50           /* BBC runs at 50 Hz (PAL)                 */

/* -------------------------------------------------------------------------
 * BBC colour palette  index → SDL RGBA8888
 * SDL_PIXELFORMAT_RGBA8888: Rmask=FF000000 Gmask=00FF0000 Bmask=0000FF00 Amask=000000FF
 * BBC bit0=R, bit1=G, bit2=B  (VideoULA convention)
 * Format: 0xRRGGBBAA
 * ------------------------------------------------------------------------- */
static const uint32_t s_palette[8] = {
    0x000000FF,   /* 0 Black   */
    0xFF0000FF,   /* 1 Red     */
    0x00FF00FF,   /* 2 Green   */
    0xFFFF00FF,   /* 3 Yellow  */
    0x0000FFFF,   /* 4 Blue    */
    0xFF00FFFF,   /* 5 Magenta */
    0x00FFFFFF,   /* 6 Cyan    */
    0xFFFFFFFF,   /* 7 White   */
};

/* -------------------------------------------------------------------------
 * Disk context
 * ------------------------------------------------------------------------- */
#define BBC_SECTORS_PER_TRACK  10
#define BBC_SECTOR_SIZE        256
#define BBC_TRACKS             80

typedef struct {
    FILE   *fp;
    bool    is_dsd;
    bool    read_only;
} disk_ctx_t;

static disk_ctx_t s_disk;

static int disk_read(void *ctx,
                     uint8_t _drive, uint8_t track, uint8_t sector,
                     uint8_t side, uint8_t _density,
                     uint8_t *buf, uint16_t *len)
{
    (void)_drive; (void)_density;
    disk_ctx_t *d = (disk_ctx_t *)ctx;
    if (!d || !d->fp) return -1;
    if (track >= BBC_TRACKS || sector >= BBC_SECTORS_PER_TRACK) return -1;
    if (!d->is_dsd && side != 0) return -1;

    long off = d->is_dsd
        ? (long)((track * 2 + side) * BBC_SECTORS_PER_TRACK + sector) * BBC_SECTOR_SIZE
        : (long)(track * BBC_SECTORS_PER_TRACK + sector) * BBC_SECTOR_SIZE;

    if (fseek(d->fp, off, SEEK_SET) != 0) return -1;
    if (fread(buf, 1, BBC_SECTOR_SIZE, d->fp) != BBC_SECTOR_SIZE) return -1;
    *len = BBC_SECTOR_SIZE;
    return 0;
}

static int disk_write(void *ctx,
                      uint8_t _drive, uint8_t track, uint8_t sector,
                      uint8_t side, uint8_t _density, bool _deleted,
                      const uint8_t *buf, uint16_t len)
{
    (void)_drive; (void)_density; (void)_deleted;
    disk_ctx_t *d = (disk_ctx_t *)ctx;
    if (!d || !d->fp || d->read_only) return -1;
    if (track >= BBC_TRACKS || sector >= BBC_SECTORS_PER_TRACK) return -1;
    if (!d->is_dsd && side != 0) return -1;
    if (len != BBC_SECTOR_SIZE) return -1;

    long off = d->is_dsd
        ? (long)((track * 2 + side) * BBC_SECTORS_PER_TRACK + sector) * BBC_SECTOR_SIZE
        : (long)(track * BBC_SECTORS_PER_TRACK + sector) * BBC_SECTOR_SIZE;

    if (fseek(d->fp, off, SEEK_SET) != 0) return -1;
    if (fwrite(buf, 1, BBC_SECTOR_SIZE, d->fp) != BBC_SECTOR_SIZE) return -1;
    fflush(d->fp);
    return 0;
}

static void disk_seek(void *_ctx, uint8_t _drive, uint8_t _track)
{
    (void)_ctx; (void)_drive; (void)_track;
}

/* -------------------------------------------------------------------------
 * ROM loader
 * ------------------------------------------------------------------------- */
static uint8_t *load_rom(const char *path, uint32_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open ROM: %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_size = (uint32_t)sz;
    return buf;
}

/* -------------------------------------------------------------------------
 * BBC keyboard matrix: SDL scancode → (row, col)
 * Same layout as main.cpp vk_to_bbc()
 * ------------------------------------------------------------------------- */
typedef struct { int row; int col; } BbcKey;

static BbcKey sdl_to_bbc(SDL_Scancode sc)
{
    switch (sc) {
    /* Row 0 */
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:   return (BbcKey){0,0};
    case SDL_SCANCODE_Q:        return (BbcKey){0,1};
    case SDL_SCANCODE_F10:      return (BbcKey){0,2};  /* F0 */
    case SDL_SCANCODE_1:        return (BbcKey){0,3};
    case SDL_SCANCODE_CAPSLOCK: return (BbcKey){0,4};
    case SDL_SCANCODE_TAB:      return (BbcKey){0,6};
    case SDL_SCANCODE_ESCAPE:   return (BbcKey){0,7};
    /* Row 1 */
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:    return (BbcKey){1,0};
    case SDL_SCANCODE_3:        return (BbcKey){1,1};
    case SDL_SCANCODE_W:        return (BbcKey){1,2};
    case SDL_SCANCODE_2:        return (BbcKey){1,3};
    case SDL_SCANCODE_A:        return (BbcKey){1,4};
    case SDL_SCANCODE_S:        return (BbcKey){1,5};
    case SDL_SCANCODE_Z:        return (BbcKey){1,6};
    /* Row 2 */
    case SDL_SCANCODE_4:        return (BbcKey){2,1};
    case SDL_SCANCODE_E:        return (BbcKey){2,2};
    case SDL_SCANCODE_R:        return (BbcKey){2,3};
    case SDL_SCANCODE_D:        return (BbcKey){2,4};
    case SDL_SCANCODE_F:        return (BbcKey){2,5};
    case SDL_SCANCODE_X:        return (BbcKey){2,6};
    case SDL_SCANCODE_C:        return (BbcKey){2,7};
    /* Row 3 */
    case SDL_SCANCODE_5:        return (BbcKey){3,1};
    case SDL_SCANCODE_T:        return (BbcKey){3,2};
    case SDL_SCANCODE_6:        return (BbcKey){3,3};
    case SDL_SCANCODE_G:        return (BbcKey){3,4};
    case SDL_SCANCODE_H:        return (BbcKey){3,5};
    case SDL_SCANCODE_V:        return (BbcKey){3,6};
    case SDL_SCANCODE_B:        return (BbcKey){3,7};
    /* Row 4 */
    case SDL_SCANCODE_F4:       return (BbcKey){4,1};
    case SDL_SCANCODE_7:        return (BbcKey){4,2};
    case SDL_SCANCODE_8:        return (BbcKey){4,3};
    case SDL_SCANCODE_Y:        return (BbcKey){4,4};
    case SDL_SCANCODE_J:        return (BbcKey){4,5};
    case SDL_SCANCODE_N:        return (BbcKey){4,6};
    case SDL_SCANCODE_SPACE:    return (BbcKey){4,7};
    /* Row 5 */
    case SDL_SCANCODE_F5:       return (BbcKey){5,1};
    case SDL_SCANCODE_I:        return (BbcKey){5,2};
    case SDL_SCANCODE_O:        return (BbcKey){5,3};
    case SDL_SCANCODE_U:        return (BbcKey){5,4};
    case SDL_SCANCODE_K:        return (BbcKey){5,5};
    case SDL_SCANCODE_M:        return (BbcKey){5,6};
    case SDL_SCANCODE_COMMA:    return (BbcKey){5,7};
    /* Row 6 */
    case SDL_SCANCODE_F6:       return (BbcKey){6,1};
    case SDL_SCANCODE_9:        return (BbcKey){6,2};
    case SDL_SCANCODE_0:        return (BbcKey){6,3};
    case SDL_SCANCODE_P:        return (BbcKey){6,4};
    case SDL_SCANCODE_L:        return (BbcKey){6,5};
    case SDL_SCANCODE_PERIOD:   return (BbcKey){6,7};
    /* Row 7 */
    case SDL_SCANCODE_F7:       return (BbcKey){7,1};
    case SDL_SCANCODE_MINUS:    return (BbcKey){7,2};
    case SDL_SCANCODE_EQUALS:   return (BbcKey){7,3};
    case SDL_SCANCODE_APOSTROPHE: return (BbcKey){7,4}; /* @ */
    case SDL_SCANCODE_SEMICOLON:return (BbcKey){7,5};
    case SDL_SCANCODE_SLASH:    return (BbcKey){7,6};
    /* Row 8 */
    case SDL_SCANCODE_F1:       return (BbcKey){8,0};
    case SDL_SCANCODE_F2:       return (BbcKey){8,1};
    case SDL_SCANCODE_F3:       return (BbcKey){8,2};
    case SDL_SCANCODE_F12:      return (BbcKey){8,3};  /* BREAK */
    case SDL_SCANCODE_UP:       return (BbcKey){8,5};
    case SDL_SCANCODE_DELETE:
    case SDL_SCANCODE_BACKSPACE:return (BbcKey){8,7};
    /* Row 9 */
    case SDL_SCANCODE_END:      return (BbcKey){9,3};  /* COPY */
    case SDL_SCANCODE_RIGHT:    return (BbcKey){9,5};
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: return (BbcKey){9,6};
    /* Left / Down not in BBC matrix — map to cursor via shift */
    case SDL_SCANCODE_LEFT:     return (BbcKey){9,5};  /* same wire, use shift */
    case SDL_SCANCODE_DOWN:     return (BbcKey){8,5};
    default:                    return (BbcKey){-1,-1};
    }
}

/* -------------------------------------------------------------------------
 * Emulator thread
 * ------------------------------------------------------------------------- */
static bbc_machine_t  *s_machine = NULL;
static volatile bool   s_running = true;

/* cycles budget per ms tick — BBC 2 MHz / 1000 */
#define CYCLES_PER_MS  2000

static void *emu_thread(void *arg)
{
    (void)arg;
    bbc_machine_reset(s_machine);
    printf("[emu] BBC Micro reset, running...\n");

    while (s_running) {
        int budget = CYCLES_PER_MS;
        while (budget > 0)
            budget -= bbc_machine_step(s_machine);
        /* yield ~1 ms */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * SDL audio callback
 * ------------------------------------------------------------------------- */
static void audio_callback(void *userdata, uint8_t *stream, int len)
{
    bbc_machine_t *m = (bbc_machine_t *)userdata;
    int n_samples = len / 2;   /* int16_t stereo → pairs; we use mono */
    int16_t *out  = (int16_t *)stream;

    /* render mono into first half, then duplicate to stereo */
    sn76489_render(&m->psg, out, (uint32_t)n_samples);

    /* duplicate mono → stereo (SDL expects stereo here if channels==2) */
    /* We open with channels=1 so this is already correct */
    (void)out;
}

/* -------------------------------------------------------------------------
 * Pixel row buffer
 * ------------------------------------------------------------------------- */
static uint8_t s_row_buf[WIN_W];

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    /* --- ROMs --------------------------------------------------------------- */
    /* Look for ROMs relative to the executable's location, then repo root */
    char os_path[512], basic_path[512];
    snprintf(os_path,    sizeof(os_path),    "../../rom/os12.rom");
    snprintf(basic_path, sizeof(basic_path), "../../rom/basic2.rom");

    uint32_t os_size = 0, basic_size = 0;
    uint8_t *os_rom    = load_rom(os_path,    &os_size);
    uint8_t *basic_rom = load_rom(basic_path, &basic_size);
    if (!os_rom || !basic_rom) {
        fprintf(stderr, "Failed to load ROMs from %s and %s\n",
                os_path, basic_path);
        fprintf(stderr, "Run from src/host/ or pass correct paths.\n");
        return 1;
    }
    printf("[rom] OS: %u B  BASIC: %u B\n", os_size, basic_size);

    /* --- Machine ------------------------------------------------------------ */
    s_machine = (bbc_machine_t *)calloc(1, sizeof(bbc_machine_t));
    if (!s_machine) { fprintf(stderr, "OOM\n"); return 1; }

    bbc_machine_init(s_machine, os_rom, os_size, basic_rom, basic_size);

    /* --- Disk --------------------------------------------------------------- */
    memset(&s_disk, 0, sizeof(s_disk));
    if (argc >= 2) {
        const char *img = argv[1];
        size_t nlen = strlen(img);
        bool is_dsd = (nlen >= 4 &&
                       (strcmp(img + nlen - 4, ".dsd") == 0 ||
                        strcmp(img + nlen - 4, ".DSD") == 0));
        s_disk.fp       = fopen(img, "r+b");
        s_disk.is_dsd   = is_dsd;
        s_disk.read_only = false;
        if (!s_disk.fp) {
            s_disk.fp        = fopen(img, "rb");
            s_disk.read_only = true;
        }
        if (s_disk.fp) {
            printf("[disk] %s (%s, %s)\n", img,
                   is_dsd ? "DSD" : "SSD",
                   s_disk.read_only ? "read-only" : "read-write");
            bbc_machine_mount_disk(s_machine, 0,
                                   disk_read, disk_write, disk_seek,
                                   &s_disk);
        } else {
            fprintf(stderr, "[disk] Cannot open %s\n", img);
        }
    }

    /* --- SDL init ----------------------------------------------------------- */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *win = SDL_CreateWindow(
        "BBC Micro (esp-beep host)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }

    /* macOS: raise window and request keyboard focus explicitly.
     * Without this, a terminal-launched SDL app may not receive key events. */
    SDL_RaiseWindow(win);

    SDL_Renderer *ren = SDL_CreateRenderer(
        win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        /* fallback to software */
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }
    SDL_RenderSetLogicalSize(ren, WIN_W, WIN_H);

    SDL_Texture *tex = SDL_CreateTexture(
        ren,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_STREAMING,
        WIN_W, WIN_H);
    if (!tex) { fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError()); return 1; }

    /* --- SDL audio ---------------------------------------------------------- */
    sn76489_init(&s_machine->psg, AUDIO_RATE);

    SDL_AudioSpec want = {0}, have = {0};
    want.freq     = AUDIO_RATE;
    want.format   = AUDIO_S16LSB;
    want.channels = 1;
    want.samples  = AUDIO_FRAMES;
    want.callback = audio_callback;
    want.userdata = s_machine;

    SDL_AudioDeviceID audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio_dev == 0) {
        fprintf(stderr, "[audio] SDL_OpenAudioDevice: %s — running without audio\n",
                SDL_GetError());
    } else {
        printf("[audio] %d Hz, %d ch, %d frames\n",
               have.freq, have.channels, have.samples);
        SDL_PauseAudioDevice(audio_dev, 0);
    }

    /* --- Emulator thread ---------------------------------------------------- */
    pthread_t emu_tid;
    pthread_create(&emu_tid, NULL, emu_thread, NULL);

    /* --- Main loop ---------------------------------------------------------- */
    printf("[host] running — close window or press Ctrl+C to quit\n");

    uint32_t *pixels = (uint32_t *)malloc(WIN_W * WIN_H * sizeof(uint32_t));
    if (!pixels) { fprintf(stderr, "OOM pixels\n"); return 1; }

    while (s_running) {
        /* Events */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                s_running = false;
            } else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
                /* Ignore key-repeat events — BBC handles repetition itself */
                if (ev.key.repeat) continue;
                /* Ctrl+Q = quit */
                if (ev.type == SDL_KEYDOWN &&
                    ev.key.keysym.scancode == SDL_SCANCODE_Q &&
                    (ev.key.keysym.mod & KMOD_CTRL)) {
                    s_running = false;
                    break;
                }
                BbcKey k = sdl_to_bbc(ev.key.keysym.scancode);
                if (k.row >= 0)
                    bbc_machine_key_event(s_machine,
                                         (uint8_t)k.row, (uint8_t)k.col,
                                         ev.type == SDL_KEYDOWN);
            }
        }

        /* Render frame: call render_row for each VGA scanline */
        for (int y = 0; y < WIN_H; y++) {
            bbc_video_render_row(&s_machine->video, y, WIN_H,
                                 s_row_buf, WIN_W);
            uint32_t *row = pixels + y * WIN_W;
            for (int x = 0; x < WIN_W; x++)
                row[x] = s_palette[s_row_buf[x] & 7];
        }

        SDL_UpdateTexture(tex, NULL, pixels, WIN_W * (int)sizeof(uint32_t));
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
    }

    /* --- Cleanup ------------------------------------------------------------ */
    s_running = false;
    pthread_join(emu_tid, NULL);

    free(pixels);
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();

    if (s_disk.fp) fclose(s_disk.fp);
    free(s_machine);
    free(os_rom);
    free(basic_rom);

    return 0;
}
