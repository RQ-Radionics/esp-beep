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
 * Run:   ./bbc-beep [disk.ssd|disk.dsd] [tape.uef]
 *        Arguments can be given in any order; extension determines type.
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
#define BBC_TRACKS_MAX         80   /* maximum supported tracks */

typedef struct {
    FILE    *fp;
    bool     is_dsd;
    bool     read_only;
    uint8_t  n_tracks;   /* actual track count read from DFS catalogue */
} disk_ctx_t;

static disk_ctx_t s_disk;

/* Read the total-sector count from the DFS catalogue in sector 1 ($100-$1FF)
 * and derive n_tracks.  Falls back to BBC_TRACKS_MAX on any error. */
static uint8_t disk_detect_tracks(FILE *fp, bool is_dsd)
{
    (void)is_dsd;
    uint8_t s1[256];
    if (fseek(fp, 256, SEEK_SET) != 0) return BBC_TRACKS_MAX;
    if (fread(s1, 1, 256, fp) != 256)  return BBC_TRACKS_MAX;
    /* DFS catalogue sector 1:
     *   byte $06 bits 1:0 = total sectors high 2 bits
     *   byte $07           = total sectors low 8 bits  */
    uint16_t total_sectors = ((uint16_t)(s1[6] & 0x03) << 8) | s1[7];
    if (total_sectors == 0) return BBC_TRACKS_MAX;
    uint8_t tracks = (uint8_t)(total_sectors / BBC_SECTORS_PER_TRACK);
    if (tracks == 0 || tracks > BBC_TRACKS_MAX) return BBC_TRACKS_MAX;
    return tracks;
}

static int disk_read(void *ctx,
                     uint8_t _drive, uint8_t track, uint8_t sector,
                     uint8_t side, uint8_t _density,
                     uint8_t *buf, uint16_t *len)
{
    (void)_drive; (void)_density;
    disk_ctx_t *d = (disk_ctx_t *)ctx;
    if (!d || !d->fp) return -1;
    if (track >= d->n_tracks || sector >= BBC_SECTORS_PER_TRACK) return -1;
    /* For SSD (single-sided), force side=0. */
    if (!d->is_dsd) side = 0;

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
    if (track >= d->n_tracks || sector >= BBC_SECTORS_PER_TRACK) return -1;
    if (!d->is_dsd) side = 0;
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
 *
 * Layout matches B-em / BBC Advanced User Guide physical layout.
 * BBC keycode = (row << 4) | col where:
 *
 *       col:  0     1     2     3     4     5     6     7     8     9
 * row 0:   Shift  Ctrl  --DIP--  --DIP--  --DIP--  --DIP--  --DIP--  --DIP--
 * row 1:   Q      3     4     5     f4    8     f7    -=    ~^    Left
 * row 2:   f0     W     E     T     7     I     9     0     £     Down
 * row 3:   1      2     D     R     6     U     O     P     [{    Up
 * row 4:   Caps   A     X     F     Y     J     K     @     :*    Return
 * row 5:   ShLk   S     C     G     H     N     L     ;+    ]}    Delete
 * row 6:   Tab    Z     Spc   V     B     M     <,    >.    /?    Copy
 * row 7:   Esc    f1    f2    f3    f5    f6    f8    f9    \|    Right
 * ------------------------------------------------------------------------- */
typedef struct { int row; int col; } BbcKey;

static BbcKey sdl_to_bbc(SDL_Scancode sc)
{
    switch (sc) {
    /* Row 0: Shift, Ctrl */
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:    return (BbcKey){0, 0};
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:     return (BbcKey){0, 1};
    /* Row 1 */
    case SDL_SCANCODE_Q:         return (BbcKey){1, 0};
    case SDL_SCANCODE_3:         return (BbcKey){1, 1};
    case SDL_SCANCODE_4:         return (BbcKey){1, 2};
    case SDL_SCANCODE_5:         return (BbcKey){1, 3};
    case SDL_SCANCODE_F4:        return (BbcKey){1, 4};
    case SDL_SCANCODE_8:         return (BbcKey){1, 5};
    case SDL_SCANCODE_F7:        return (BbcKey){1, 6};
    case SDL_SCANCODE_MINUS:     return (BbcKey){1, 7};
    case SDL_SCANCODE_LEFT:      return (BbcKey){1, 9};
    /* Row 2 */
    case SDL_SCANCODE_F10:       return (BbcKey){2, 0};  /* f0 */
    case SDL_SCANCODE_W:         return (BbcKey){2, 1};
    case SDL_SCANCODE_E:         return (BbcKey){2, 2};
    case SDL_SCANCODE_T:         return (BbcKey){2, 3};
    case SDL_SCANCODE_7:         return (BbcKey){2, 4};
    case SDL_SCANCODE_I:         return (BbcKey){2, 5};
    case SDL_SCANCODE_9:         return (BbcKey){2, 6};
    case SDL_SCANCODE_0:         return (BbcKey){2, 7};
    case SDL_SCANCODE_DOWN:      return (BbcKey){2, 9};
    /* Row 3 */
    case SDL_SCANCODE_1:         return (BbcKey){3, 0};
    case SDL_SCANCODE_2:         return (BbcKey){3, 1};
    case SDL_SCANCODE_D:         return (BbcKey){3, 2};
    case SDL_SCANCODE_R:         return (BbcKey){3, 3};
    case SDL_SCANCODE_6:         return (BbcKey){3, 4};
    case SDL_SCANCODE_U:         return (BbcKey){3, 5};
    case SDL_SCANCODE_O:         return (BbcKey){3, 6};
    case SDL_SCANCODE_P:         return (BbcKey){3, 7};
    case SDL_SCANCODE_UP:        return (BbcKey){3, 9};
    /* Row 4 */
    case SDL_SCANCODE_CAPSLOCK:  return (BbcKey){4, 0};
    case SDL_SCANCODE_A:         return (BbcKey){4, 1};
    case SDL_SCANCODE_X:         return (BbcKey){4, 2};
    case SDL_SCANCODE_F:         return (BbcKey){4, 3};
    case SDL_SCANCODE_Y:         return (BbcKey){4, 4};
    case SDL_SCANCODE_J:         return (BbcKey){4, 5};
    case SDL_SCANCODE_K:         return (BbcKey){4, 6};
    /* US keyboard layout:
     *   '  (apostrophe) → BBC :* (row4 col8).  Shift+' = * on BBC.
     *   `  (grave)      → BBC @  (row4 col7).
     *   ;  (semicolon)  → BBC ;+ (row5 col7) — already below.
     * The BBC @ key has no direct US equivalent; grave is the closest spare. */
    case SDL_SCANCODE_APOSTROPHE:return (BbcKey){4, 8};  /* : / * */
    case SDL_SCANCODE_GRAVE:     return (BbcKey){4, 7};  /* @ */
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:  return (BbcKey){4, 9};
    /* Row 5 */
    /* ShiftLock = col 0 row 5 — no standard SDL key */
    case SDL_SCANCODE_S:         return (BbcKey){5, 1};
    case SDL_SCANCODE_C:         return (BbcKey){5, 2};
    case SDL_SCANCODE_G:         return (BbcKey){5, 3};
    case SDL_SCANCODE_H:         return (BbcKey){5, 4};
    case SDL_SCANCODE_N:         return (BbcKey){5, 5};
    case SDL_SCANCODE_L:         return (BbcKey){5, 6};
    case SDL_SCANCODE_SEMICOLON: return (BbcKey){5, 7};
    case SDL_SCANCODE_DELETE:
    case SDL_SCANCODE_BACKSPACE: return (BbcKey){5, 9};
    /* Row 6 */
    case SDL_SCANCODE_TAB:       return (BbcKey){6, 0};
    case SDL_SCANCODE_Z:         return (BbcKey){6, 1};
    case SDL_SCANCODE_SPACE:     return (BbcKey){6, 2};
    case SDL_SCANCODE_V:         return (BbcKey){6, 3};
    case SDL_SCANCODE_B:         return (BbcKey){6, 4};
    case SDL_SCANCODE_M:         return (BbcKey){6, 5};
    case SDL_SCANCODE_COMMA:     return (BbcKey){6, 6};
    case SDL_SCANCODE_PERIOD:    return (BbcKey){6, 7};
    case SDL_SCANCODE_SLASH:     return (BbcKey){6, 8};
    case SDL_SCANCODE_END:       return (BbcKey){6, 9};  /* Copy */
    /* Row 7 */
    case SDL_SCANCODE_ESCAPE:    return (BbcKey){7, 0};
    case SDL_SCANCODE_F1:        return (BbcKey){7, 1};
    case SDL_SCANCODE_F2:        return (BbcKey){7, 2};
    case SDL_SCANCODE_F3:        return (BbcKey){7, 3};
    case SDL_SCANCODE_F5:        return (BbcKey){7, 4};
    case SDL_SCANCODE_F6:        return (BbcKey){7, 5};
    case SDL_SCANCODE_F8:        return (BbcKey){7, 6};
    case SDL_SCANCODE_F9:        return (BbcKey){7, 7};
    case SDL_SCANCODE_BACKSLASH: return (BbcKey){7, 8};
    case SDL_SCANCODE_RIGHT:     return (BbcKey){7, 9};
    /* Extra keys (US layout)
     * BBC row1 col8 = ^ / ~   → US = (no direct key; use = as fallback)
     * BBC row3 col8 = [ / {   → US [
     * BBC row5 col8 = ] / }   → US ]
     * BBC row7 col8 = \ / |   → US backslash (already above)
     * BBC row1 col7 = - / =   → US -  (already above)
     * Note: GRAVE is now used for BBC @ (row4 col7) */
    case SDL_SCANCODE_EQUALS:       return (BbcKey){1, 8};  /* ^ / ~ on BBC */
    case SDL_SCANCODE_LEFTBRACKET:  return (BbcKey){3, 8};  /* [ / { */
    case SDL_SCANCODE_RIGHTBRACKET: return (BbcKey){5, 8};  /* ] / } */
    default:                     return (BbcKey){-1, -1};
    }
}

/* -------------------------------------------------------------------------
 * Autorun key injection
 *
 * Each entry represents one key event (press or release) to be injected
 * after a delay_cycles BBC CPU cycles have elapsed since the previous event.
 * This gives the MOS time to scan the keyboard matrix and register the key.
 *
 * Typical BBC keyboard scan runs at ~50 Hz (every 40000 cycles at 2 MHz).
 * A safe press-hold time is 3 scans = ~120000 cycles, release after that.
 * Between distinct characters we add an extra 80000-cycle gap.
 * ------------------------------------------------------------------------- */
#define AUTORUN_MAX_EVENTS 512

typedef struct {
    int      row;          /* BBC matrix row (0-7), -1 = no-op delay */
    int      col;          /* BBC matrix col (0-9) */
    bool     pressed;      /* true = key down, false = key up */
    int32_t  delay_cycles; /* wait this many cycles before firing */
} AutorunEvent;

static AutorunEvent  s_autorun[AUTORUN_MAX_EVENTS];
static int           s_autorun_count = 0;

/* Convert an ASCII character to BBC matrix (row,col) + needs_shift.
 * Returns false for unmapped characters. */
static bool ascii_to_bbc(char c, int *row, int *col, bool *shift)
{
    *shift = false;
    switch (c) {
    /* Letters */
    case 'a': case 'A': *row=4; *col=1; *shift=(c>='A'&&c<='Z'); return true;
    case 'b': case 'B': *row=6; *col=4; *shift=(c>='A'&&c<='Z'); return true;
    case 'c': case 'C': *row=5; *col=2; *shift=(c>='A'&&c<='Z'); return true;
    case 'd': case 'D': *row=3; *col=2; *shift=(c>='A'&&c<='Z'); return true;
    case 'e': case 'E': *row=2; *col=2; *shift=(c>='A'&&c<='Z'); return true;
    case 'f': case 'F': *row=4; *col=3; *shift=(c>='A'&&c<='Z'); return true;
    case 'g': case 'G': *row=5; *col=3; *shift=(c>='A'&&c<='Z'); return true;
    case 'h': case 'H': *row=5; *col=4; *shift=(c>='A'&&c<='Z'); return true;
    case 'i': case 'I': *row=2; *col=5; *shift=(c>='A'&&c<='Z'); return true;
    case 'j': case 'J': *row=4; *col=5; *shift=(c>='A'&&c<='Z'); return true;
    case 'k': case 'K': *row=4; *col=6; *shift=(c>='A'&&c<='Z'); return true;
    case 'l': case 'L': *row=5; *col=6; *shift=(c>='A'&&c<='Z'); return true;
    case 'm': case 'M': *row=6; *col=5; *shift=(c>='A'&&c<='Z'); return true;
    case 'n': case 'N': *row=5; *col=5; *shift=(c>='A'&&c<='Z'); return true;
    case 'o': case 'O': *row=3; *col=6; *shift=(c>='A'&&c<='Z'); return true;
    case 'p': case 'P': *row=3; *col=7; *shift=(c>='A'&&c<='Z'); return true;
    case 'q': case 'Q': *row=1; *col=0; *shift=(c>='A'&&c<='Z'); return true;
    case 'r': case 'R': *row=3; *col=3; *shift=(c>='A'&&c<='Z'); return true;
    case 's': case 'S': *row=5; *col=1; *shift=(c>='A'&&c<='Z'); return true;
    case 't': case 'T': *row=2; *col=3; *shift=(c>='A'&&c<='Z'); return true;
    case 'u': case 'U': *row=3; *col=5; *shift=(c>='A'&&c<='Z'); return true;
    case 'v': case 'V': *row=6; *col=3; *shift=(c>='A'&&c<='Z'); return true;
    case 'w': case 'W': *row=2; *col=1; *shift=(c>='A'&&c<='Z'); return true;
    case 'x': case 'X': *row=4; *col=2; *shift=(c>='A'&&c<='Z'); return true;
    case 'y': case 'Y': *row=4; *col=4; *shift=(c>='A'&&c<='Z'); return true;
    case 'z': case 'Z': *row=6; *col=1; *shift=(c>='A'&&c<='Z'); return true;
    /* Digits */
    case '1': *row=3; *col=0; return true;
    case '2': *row=3; *col=1; return true;
    case '3': *row=1; *col=1; return true;
    case '4': *row=1; *col=2; return true;
    case '5': *row=1; *col=3; return true;
    case '6': *row=3; *col=4; return true;
    case '7': *row=2; *col=4; return true;
    case '8': *row=1; *col=5; return true;
    case '9': *row=2; *col=6; return true;
    case '0': *row=2; *col=7; return true;
    /* Punctuation */
    case '\n': case '\r': *row=4; *col=9; return true;  /* Return */
    case ' ':  *row=6; *col=2; return true;
    case '*':  *row=4; *col=8; *shift=true; return true; /* Shift+: = * */
    case '"':  *row=3; *col=1; *shift=true; return true; /* Shift+2 = " */
    case '-':  *row=1; *col=7; return true;
    /* Symbols — BBC Micro keyboard matrix.
     * Row1: Q  3  4  5  F4 8  F7 -  =  ~   (cols 0-9)
     * Row2: W  E  T  7  I  9  0  ^  .  .
     * Row3: 1  2  D  R  6  U  O  P  [  .
     * Row4: Cp A  X  F  Y  J  K  @  :  Ret
     * Row5: Sh S  C  G  H  N  L  ;  ]  Del
     * Row6: Tb Z  Sp V  B  M  ,  .  /  .
     * Row7: Es f0 f1 f2 f3 f4 f5 f6 f7 \   */
    case '=':  *row=1; *col=7; *shift=true; return true;  /* Shift+- = = */
    case '~':  *row=1; *col=9; return true;               /* ~ own key  */
    case '^':  *row=2; *col=7; return true;               /* ^ own key  */
    case '!':  *row=3; *col=0; *shift=true; return true;  /* Shift+1    */
    case '#':  *row=1; *col=1; *shift=true; return true;  /* Shift+3    */
    case '$':  *row=1; *col=2; *shift=true; return true;  /* Shift+4    */
    case '%':  *row=1; *col=3; *shift=true; return true;  /* Shift+5    */
    case '&':  *row=3; *col=4; *shift=true; return true;  /* Shift+6    */
    case '\'': *row=2; *col=3; *shift=true; return true;  /* Shift+7 = '*/
    case '(':  *row=1; *col=5; *shift=true; return true;  /* Shift+8    */
    case ')':  *row=2; *col=6; *shift=true; return true;  /* Shift+9    */
    case '+':  *row=2; *col=5; *shift=true; return true;  /* Shift+9? no: Shift+; on BBC = + */
    case '_':  *row=1; *col=7; *shift=true; return true;  /* Shift+-    */
    case '@':  *row=4; *col=7; return true;               /* @ own key  */
    case '[':  *row=3; *col=8; return true;               /* [ own key  */
    case '{':  *row=3; *col=8; *shift=true; return true;
    case ']':  *row=5; *col=8; return true;               /* ] own key  */
    case '}':  *row=5; *col=8; *shift=true; return true;
    case ';':  *row=5; *col=7; return true;
    case ':':  *row=4; *col=8; return true;
    case '\\': *row=7; *col=8; return true;
    case '|':  *row=7; *col=8; *shift=true; return true;
    case ',':  *row=6; *col=6; return true;
    case '<':  *row=6; *col=6; *shift=true; return true;
    case '>':  *row=6; *col=7; *shift=true; return true;
    case '?':  *row=6; *col=8; *shift=true; return true;  /* Shift+/    */
    case '.':  *row=6; *col=7; return true;
    case '/':  *row=6; *col=8; return true;
    default: return false;
    }
}

/* Append one key-press+release pair (with optional shift) to s_autorun[].
 * delay_before_cycles: idle time before the press.
 * hold_cycles:         how long to hold the key down.
 */
static void autorun_append_key(int row, int col, bool shift,
                               int32_t delay_before, int32_t hold)
{
    /* Shift down (if needed) — press shift first, wait 40000 cycles before key */
    if (shift && s_autorun_count < AUTORUN_MAX_EVENTS) {
        s_autorun[s_autorun_count++] = (AutorunEvent){0, 0, true, delay_before};
        delay_before = 40000; /* one keyboard scan after shift down */
    }
    /* Key down */
    if (s_autorun_count < AUTORUN_MAX_EVENTS)
        s_autorun[s_autorun_count++] = (AutorunEvent){row, col, true, delay_before};
    /* Key up after hold */
    if (s_autorun_count < AUTORUN_MAX_EVENTS)
        s_autorun[s_autorun_count++] = (AutorunEvent){row, col, false, hold};
    /* Shift up — release shift after key up */
    if (shift && s_autorun_count < AUTORUN_MAX_EVENTS)
        s_autorun[s_autorun_count++] = (AutorunEvent){0, 0, false, 20000};
}

/* Build the autorun event list from a string.
 * The string is typed after waiting initial_delay_cycles. */
static void autorun_build(const char *text, int32_t initial_delay_cycles)
{
    /* Cycles per key: 6 keyboard scans at 50 Hz = 240000 cycles hold,
     * then 160000 cycles gap between chars = 400000 cycles per char total.
     * BBC keyboard scan period = 40000 cycles @ 2 MHz. */
    const int32_t HOLD   = 240000;  /* ~120 ms hold */
    const int32_t GAP    = 200000;  /* ~100 ms between keys */

    int32_t first_delay = initial_delay_cycles;

    for (const char *p = text; *p; p++) {
        int row, col;
        bool shift;
        if (!ascii_to_bbc(*p, &row, &col, &shift)) {
            fprintf(stderr, "[autorun] unmapped char '%c'\n", *p);
            continue;
        }
        int32_t before = (p == text) ? first_delay : GAP;
        /* Return gets extra settling time — MOS needs to process the command */
        if (*p == '\n' || *p == '\r')
            before += 400000;  /* extra ~200ms before each Return */
        autorun_append_key(row, col, shift, before, HOLD);
        /* After Return, add a long pause for MOS to execute the command.
         * If this is the last Return in the string, wait much longer —
         * the next command may be CHAIN which loads from tape (minutes). */
        if (*p == '\n' || *p == '\r') {
            int32_t post_return_wait = (*(p+1) == '\0') ? 360000000  /* 3 min */
                                                        : 2000000;   /* 1 sec */
            if (s_autorun_count < AUTORUN_MAX_EVENTS)
                s_autorun[s_autorun_count++] = (AutorunEvent){-1, 0, false, post_return_wait};
        }
    }
    fprintf(stderr, "[autorun] queued %d events for %zu chars\n",
            s_autorun_count, strlen(text));
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

    /* Autorun injection state */
    int     ar_idx  = 0;
    int32_t ar_wait = (s_autorun_count > 0) ? s_autorun[0].delay_cycles : 0;

    while (s_running) {
        int budget = CYCLES_PER_MS;
        while (budget > 0)
            budget -= bbc_machine_step(s_machine);

        /* Process autorun events */
        if (ar_idx < s_autorun_count) {
            ar_wait -= CYCLES_PER_MS;
            while (ar_idx < s_autorun_count && ar_wait <= 0) {
                AutorunEvent *ev = &s_autorun[ar_idx];
                if (ev->row >= 0)
                    bbc_machine_key_event(s_machine,
                                          (uint8_t)ev->row, (uint8_t)ev->col,
                                          ev->pressed);
                ar_idx++;
                if (ar_idx < s_autorun_count)
                    ar_wait += s_autorun[ar_idx].delay_cycles;
            }
        }

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
    char os_path[512], basic_path[512], dfs_path[512];
    snprintf(os_path,    sizeof(os_path),    "../../rom/os12.rom");
    snprintf(basic_path, sizeof(basic_path), "../../rom/basic2.rom");
    snprintf(dfs_path,   sizeof(dfs_path),   "../../rom/dfs1770.rom");

    uint32_t os_size = 0, basic_size = 0, dfs_size = 0;
    uint8_t *os_rom    = load_rom(os_path,    &os_size);
    uint8_t *basic_rom = load_rom(basic_path, &basic_size);
    uint8_t *dfs_rom   = load_rom(dfs_path,   &dfs_size);
    if (!os_rom || !basic_rom) {
        fprintf(stderr, "Failed to load ROMs from %s and %s\n",
                os_path, basic_path);
        fprintf(stderr, "Run from src/host/ or pass correct paths.\n");
        return 1;
    }
    printf("[rom] OS: %u B  BASIC: %u B  DFS: %u B\n",
           os_size, basic_size, dfs_size);

    /* --- Machine ------------------------------------------------------------ */
    s_machine = (bbc_machine_t *)calloc(1, sizeof(bbc_machine_t));
    if (!s_machine) { fprintf(stderr, "OOM\n"); return 1; }

    bbc_machine_init(s_machine, os_rom, os_size, basic_rom, basic_size);

    /* Load DFS ROM into sideways slot 14 (slot 15 = BASIC, 14 = DFS) */
    if (dfs_rom && dfs_size)
        bbc_machine_load_sideways_rom(s_machine, dfs_rom, dfs_size, 14);

    /* --- Disk + Tape -------------------------------------------------------- */
    memset(&s_disk, 0, sizeof(s_disk));

    /* Helper: does 'path' end with 'ext' (case-insensitive)? */
    #define HAS_EXT(path, ext) \
        (strlen(path) >= strlen(ext) && \
         strcasecmp((path) + strlen(path) - strlen(ext), (ext)) == 0)

    const char *autorun_str = NULL;
    int32_t     autorun_delay = 6000000; /* 3 seconds at 2 MHz before first key */

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--autorun") == 0 && i+1 < argc) {
            autorun_str = argv[++i];
            continue;
        } else if (strncmp(arg, "--autorun=", 10) == 0) {
            autorun_str = arg + 10;
            continue;
        }

        if (HAS_EXT(arg, ".uef")) {
            /* --- Tape image ------------------------------------------------ */
            printf("[tape] mounting %s\n", arg);
            if (bbc_machine_mount_tape(s_machine, arg) != 0)
                fprintf(stderr, "[tape] Failed to load %s\n", arg);

        } else if (HAS_EXT(arg, ".ssd") || HAS_EXT(arg, ".dsd")) {
            /* --- Disk image ------------------------------------------------ */
            if (s_disk.fp) {
                fprintf(stderr, "[disk] Only one disk image supported; ignoring %s\n", arg);
                continue;
            }
            bool is_dsd = HAS_EXT(arg, ".dsd");
            s_disk.fp       = fopen(arg, "r+b");
            s_disk.is_dsd   = is_dsd;
            s_disk.read_only = false;
            if (!s_disk.fp) {
                s_disk.fp        = fopen(arg, "rb");
                s_disk.read_only = true;
            }
            if (s_disk.fp) {
                s_disk.n_tracks = disk_detect_tracks(s_disk.fp, is_dsd);
                printf("[disk] %s (%s, %s, %u tracks)\n", arg,
                       is_dsd ? "DSD" : "SSD",
                       s_disk.read_only ? "read-only" : "read-write",
                       s_disk.n_tracks);
                bbc_machine_mount_disk(s_machine, 0,
                                       disk_read, disk_write, disk_seek,
                                       &s_disk);
            } else {
                fprintf(stderr, "[disk] Cannot open %s\n", arg);
            }

        } else {
            fprintf(stderr, "[args] Unknown file type: %s (expected .ssd/.dsd/.uef)\n", arg);
        }
    }
    #undef HAS_EXT

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

    /* --- Autorun ------------------------------------------------------------ */
    if (autorun_str)
        autorun_build(autorun_str, autorun_delay);

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
                /* F12 (or F11) = BBC BREAK key (soft reset, RAM preserved).
                 * Shift+F12 / Shift+F11 = SHIFT+BREAK (disc autoboot). */
                SDL_Scancode sc = ev.key.keysym.scancode;
                if (ev.type == SDL_KEYDOWN &&
                    (sc == SDL_SCANCODE_F12 || sc == SDL_SCANCODE_F11)) {
                    bool shift = (ev.key.keysym.mod & KMOD_SHIFT) != 0;
                    printf("[host] BREAK%s\n", shift ? " (SHIFT)" : "");
                    bbc_machine_break(s_machine, shift);
                    continue;
                }
                BbcKey k = sdl_to_bbc(ev.key.keysym.scancode);
                if (k.row >= 0) {
                    bbc_machine_key_event(s_machine,
                                         (uint8_t)k.row, (uint8_t)k.col,
                                         ev.type == SDL_KEYDOWN);
                } else if (ev.type == SDL_KEYDOWN) {
                    /* Unmapped key: print scancode to help diagnose layout */
                    printf("[key] unmapped scancode=%d sym=%d name=%s\n",
                           (int)ev.key.keysym.scancode,
                           (int)ev.key.keysym.sym,
                           SDL_GetKeyName(ev.key.keysym.sym));
                }
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
