/*
 * bbc_machine.h — BBC Micro Model B top-level integration
 *
 * Holds all hardware components and wires them together:
 *
 *   CPU (6502)       ← bbc_cpu
 *   Memory           ← bbc_memory  (32 KB RAM + ROMs + I/O callbacks)
 *   System VIA IC3   ← bbc_sysvia  (&FE40–&FE4F)
 *   User VIA IC69    ← bbc_uservia (&FE60–&FE6F)
 *   CRTC MC6845      ← bbc_video   (&FE00–&FE01)
 *   Video ULA        ← bbc_video   (&FE20–&FE21)
 *   WD1770 FDC       ← wd1770      (&FE80–&FE83, &FE84 drive select)
 *   SN76489 PSG      ← sn76489     (written via IC32 latch bit 0 LOW)
 *
 * Signal routing:
 *   VSYNC  → bbc_sysvia_vsync()  → CA1  → IFR.1 → /IRQ
 *   sysvia IRQ                   → bbc_cpu_irq()
 *   uservia IRQ                  → bbc_cpu_irq()
 *   WD1770 INTRQ                 → bbc_cpu_nmi()
 *   IC32 SOUND_WE (bit 0) LOW    → sn76489_write() with sysvia port-A data
 *
 * Usage:
 *
 *   static bbc_machine_t machine;
 *   bbc_machine_init(&machine, os_rom, os_size, basic_rom, basic_size);
 *   bbc_machine_reset(&machine);
 *
 *   // In emulator loop:
 *   int cycles = bbc_machine_step(&machine);   // step CPU + tick peripherals
 *
 *   // To mount a disk image:
 *   bbc_machine_mount_disk(&machine, 0, read_cb, write_cb, seek_cb, disk_ctx);
 *
 *   // To push a key event:
 *   bbc_machine_key_event(&machine, row, col, pressed);
 *
 * Licence: zlib
 * Copyright (c) 2026 esp-beep project
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "bbc_memory.h"
#include "bbc_cpu.h"
#include "bbc_sysvia.h"
#include "bbc_uservia.h"
#include "wd1770.h"
#include "sn76489.h"
#include "bbc_video.h"
#include "bbc_tape.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * Keyboard matrix
 *
 * BBC Model B keyboard: 8 rows × 10 columns.
 * MOS writes (row<<4)|col to Port A; row = bits 6:4, col = bits 3:0.
 * Row 0: Shift(col0), Ctrl(col1)
 * Rows 1-7: letter/number/function keys (cols 0-9)
 * ------------------------------------------------------------------ */
#define BBC_KB_ROWS   8
#define BBC_KB_COLS  10

typedef struct {
    bool pressed[BBC_KB_ROWS][BBC_KB_COLS];
} bbc_keyboard_t;

/* ------------------------------------------------------------------
 * IRQ tracking — multiple sources share /IRQ line
 * ------------------------------------------------------------------ */
typedef struct {
    bool sysvia;
    bool uservia;
} bbc_irq_state_t;

/* ------------------------------------------------------------------
 * Main machine struct
 * ------------------------------------------------------------------ */
typedef struct {
    /* Core */
    BBCMemory       *mem;
    BBCCPU          *cpu;

    /* Peripherals (stack-allocated, no heap) */
    bbc_sysvia_t     sysvia;
    bbc_uservia_t    uservia;
    wd1770_t         fdc;
    sn76489_t        psg;
    bbc_video_t      video;

    /* Keyboard */
    bbc_keyboard_t   keyboard;

    /* IRQ line state */
    bbc_irq_state_t  irq;

    /* Cycle accumulators for peripheral ticking */
    int32_t          cycle_acc;
    int32_t          crtc_acc;   /* sub-cycle acc for CRTC 1 MHz tick */

    /* FDC DRQ latch state — readable at &FE84 bit 7 (active LOW).
     * The Acorn 1770 DFS NMI handler reads &FE84 to distinguish INTRQ
     * (command complete) from DRQ (data byte ready). bit7=0 → DRQ active. */
    bool             fdc_drq_state;

    /* Last value written to the &FE84 drive-select latch.
     * Bits 0-3 are readable back (drive/side/density selects). */
    uint8_t          fdc_latch;

    /* Disk I/O callbacks — stored separately from fdc.cb so that
     * fdc.cb.user_ctx can always point to 'm' (needed by fdc_irq/fdc_drq).
     * bbc_machine_mount_disk() stores the host callbacks here; the FDC
     * wrapper functions (disk_read_wrap etc.) forward to these. */
    int  (*disk_read_sector) (void *, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t *, uint16_t *);
    int  (*disk_write_sector)(void *, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, bool, const uint8_t *, uint16_t);
    void (*disk_seek)        (void *, uint8_t, uint8_t);
    void  *disk_ctx;

    /* Tape (UEF) */
    bbc_tape_t       tape;

    /* Video framebuffer output (set by caller before init) */
    bbc_video_output_t *fb_output;   /* NULL = no display */

    /* Optional frame-ready callback */
    void (*on_frame)(void *ctx);
    void  *on_frame_ctx;
} bbc_machine_t;

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

/*
 * Initialise the machine.  Loads ROMs, wires all callbacks, resets
 * everything.  Does NOT call bbc_machine_reset() — call that separately
 * after mounting disks / configuring the framebuffer.
 */
void bbc_machine_init(bbc_machine_t *m,
                      const uint8_t *os_rom,    uint32_t os_size,
                      const uint8_t *basic_rom, uint32_t basic_size);

/*
 * Load a ROM into a specific sideways slot (0–15) after init.
 * Must be called after bbc_machine_init().
 * Conventional slots:  15 = BASIC,  14 = DFS.
 * romSize may be 8 KB (e.g. DFS 0.9) or 16 KB.
 */
void bbc_machine_load_sideways_rom(bbc_machine_t *m,
                                    const uint8_t *rom_data, uint32_t rom_size,
                                    uint8_t slot);

/* Hard reset — all chips reset, CPU reset vector fetched. */
void bbc_machine_reset(bbc_machine_t *m);

/*
 * Soft BREAK — resets CPU and peripherals but preserves RAM contents.
 * This matches the BBC hardware BREAK key behaviour: the 6502 /RESET line
 * is asserted but the RAM is not cleared.  Zero-page variables (including
 * $EF which the DFS uses to identify the reset type) survive, so sideways
 * ROMs can detect the difference between power-on and BREAK and print their
 * banners accordingly.
 *
 * shift_held: if true, Shift is asserted in the keyboard matrix for one
 * reset cycle so that the DFS (and BASIC) see a SHIFT+BREAK and perform
 * disc autoboot (*EXEC !BOOT).
 */
void bbc_machine_break(bbc_machine_t *m, bool shift_held);

/*
 * Step the emulator: execute one CPU instruction and tick all
 * peripherals by the resulting number of cycles.
 * Returns the number of cycles consumed.
 */
int bbc_machine_step(bbc_machine_t *m);

/*
 * Mount a disk image on the WD1770.  Pass NULL callbacks to unmount.
 *   drive   0 or 1
 */
    /*
     * Mount a UEF tape image.  Pass NULL path to unmount.
     * Returns 0 on success, -1 on error.
     */
    int bbc_machine_mount_tape(bbc_machine_t *m, const char *uef_path);

    void bbc_machine_mount_disk(bbc_machine_t *m, uint8_t drive,
    int  (*read_sector) (void *, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t *, uint16_t *),
    int  (*write_sector)(void *, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, bool, const uint8_t *, uint16_t),
    void (*seek)        (void *, uint8_t, uint8_t),
    void *disk_ctx);

/*
 * Set / clear a key in the keyboard matrix.
 *   row  0–7, col  0–9
 */
void bbc_machine_key_event(bbc_machine_t *m, uint8_t row, uint8_t col, bool pressed);

/* Set framebuffer output (can be changed at any time) */
void bbc_machine_set_video_output(bbc_machine_t *m, bbc_video_output_t *out);

/* Set frame-ready callback */
void bbc_machine_set_frame_callback(bbc_machine_t *m,
                                     void (*cb)(void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
