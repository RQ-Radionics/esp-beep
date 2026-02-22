/*
 * bbc_machine.c — BBC Micro Model B top-level integration
 *
 * Licence: zlib
 * Copyright (c) 2026 esp-beep project
 */

#include "bbc_machine.h"
#include "bbc_tape.h"
#include <string.h>
#include <stdio.h>

/* ======================================================================
 * Forward declarations for static callback functions
 * ====================================================================== */

/* sysvia callbacks */
static void     sv_sound_write  (void *ctx, uint8_t data);
static bool     sv_keyboard_read(void *ctx, uint8_t row, uint8_t col);
static void     sv_latch_changed(void *ctx, uint8_t latch_bits);
static void     sv_irq          (void *ctx, bool state);

/* uservia callbacks */
static void     uv_port_out(void *ctx, uint8_t port, uint8_t val, uint8_t ddr);
static uint8_t  uv_port_in (void *ctx, uint8_t port);
static void     uv_irq     (void *ctx, bool state);

/* wd1770 callbacks */
static void     fdc_irq(void *ctx, bool state);
static void     fdc_drq(void *ctx, bool state);

/* tape callbacks and IO handlers */
static void     tape_irq        (void *ctx, bool state);
static void     sv_motor_changed(void *ctx, bool on);
static uint8_t  io_acia_read        (uint16_t addr, void *ctx);
static void     io_acia_write       (uint16_t addr, uint8_t val, void *ctx);
static void     io_serial_ula_write (uint16_t addr, uint8_t val, void *ctx);

/* wd1770 disk I/O wrappers (forward to m->disk_* callbacks) */
static int      fdc_read_sector (void *ctx, uint8_t drive, uint8_t track, uint8_t sector, uint8_t side, uint8_t density, uint8_t *buf, uint16_t *len);
static int      fdc_write_sector(void *ctx, uint8_t drive, uint8_t track, uint8_t sector, uint8_t side, uint8_t density, bool deleted, const uint8_t *buf, uint16_t len);
static void     fdc_seek        (void *ctx, uint8_t drive, uint8_t track);

/* video vsync callback */
static void     video_vsync_cb(void *ctx, bool state);

/* bbc_memory I/O callbacks (per Sheila address range) */
static uint8_t  io_crtc_read     (uint16_t addr, void *ctx);
static void     io_crtc_write    (uint16_t addr, uint8_t val, void *ctx);
static uint8_t  io_vidproc_read  (uint16_t addr, void *ctx);
static void     io_vidproc_write (uint16_t addr, uint8_t val, void *ctx);
static uint8_t  io_sysvia_read   (uint16_t addr, void *ctx);
static void     io_sysvia_write  (uint16_t addr, uint8_t val, void *ctx);
static uint8_t  io_uservia_read  (uint16_t addr, void *ctx);
static void     io_uservia_write (uint16_t addr, uint8_t val, void *ctx);
static uint8_t  io_fdc_read      (uint16_t addr, void *ctx);
static void     io_fdc_write     (uint16_t addr, uint8_t val, void *ctx);
static uint8_t  io_romsel_read   (uint16_t addr, void *ctx);
static void     io_romsel_write  (uint16_t addr, uint8_t val, void *ctx);

/* ======================================================================
 * IRQ helpers
 * ====================================================================== */

/* Recalculate the CPU /IRQ line from all sources.
 * All IRQ sources (sysvia, uservia, ACIA) share the same /IRQ line.
 * The line is asserted (low) if ANY source is active. */
static void update_irq(bbc_machine_t *m) {
    if (m->irq.sysvia || m->irq.uservia || m->irq.acia) {
        bbc_cpu_irq(m->cpu);
    } else {
        bbc_cpu_clear_irq(m->cpu);
    }
}

/* ======================================================================
 * bbc_machine_init
 * ====================================================================== */

void bbc_machine_init(bbc_machine_t *m,
                      const uint8_t *os_rom,    uint32_t os_size,
                      const uint8_t *basic_rom, uint32_t basic_size) {
    memset(m, 0, sizeof(*m));

    /* ----- Memory --------------------------------------------------- */
    m->mem = bbc_memory_create();

    if (os_rom    && os_size)    bbc_memory_load_rom(m->mem, os_rom,    os_size,    0xC000);
    if (basic_rom && basic_size) bbc_memory_load_rom(m->mem, basic_rom, basic_size, 0x8000);

    /* ----- CPU ------------------------------------------------------ */
    /* The CPU read/write callbacks just call through to bbc_memory.    */
    /* We store m itself as userData so the wrappers can access m->mem. */
    m->cpu = bbc_cpu_create(
        /* read  */ bbc_memory_read,
        /* write */ bbc_memory_write,
        /* ctx   */ m->mem
    );

    /* ----- System VIA ---------------------------------------------- */
    {
        bbc_sysvia_callbacks_t cb = {
            .sound_write   = sv_sound_write,
            .keyboard_read = sv_keyboard_read,
            .latch_changed = sv_latch_changed,
            .irq           = sv_irq,
            .motor_changed = sv_motor_changed,
            .user_ctx      = m,
        };
        bbc_sysvia_init(&m->sysvia, &cb);
    }

    /* ----- User VIA ------------------------------------------------- */
    {
        bbc_uservia_callbacks_t cb = {
            .port_out = uv_port_out,
            .port_in  = uv_port_in,
            .irq      = uv_irq,
            .user_ctx = m,
        };
        bbc_uservia_init(&m->uservia, &cb);
    }

    /* ----- WD1770 FDC ---------------------------------------------- */
    /* IMPORTANT: user_ctx is always 'm' so that fdc_irq / fdc_drq can
     * safely cast ctx to bbc_machine_t*.  Disk I/O callbacks are stored
     * separately in m->disk_* and forwarded via fdc_read_sector et al.
     * Never replace user_ctx — use bbc_machine_mount_disk() instead. */
    {
        wd1770_callbacks_t cb = {
            .read_sector  = fdc_read_sector,
            .write_sector = fdc_write_sector,
            .seek         = fdc_seek,
            .irq          = fdc_irq,
            .drq          = fdc_drq,
            .user_ctx     = m,
        };
        wd1770_init(&m->fdc, &cb);
    }

    /* ----- Tape (ACIA / Serial ULA) --------------------------------- */
    bbc_tape_init(&m->tape);
    bbc_tape_set_irq_cb(&m->tape, tape_irq, m);

    /* ----- SN76489 PSG --------------------------------------------- */
    sn76489_init(&m->psg, 22050);

    /* ----- Video ---------------------------------------------------- */
    {
        uint8_t *ram = bbc_memory_get_ram(m->mem);
        bbc_video_init(&m->video, ram, 32768);
        bbc_video_set_vsync_callback(&m->video, video_vsync_cb, m);
    }

    /* ----- Wire I/O callbacks into bbc_memory ----------------------- */

    /* CRTC MC6845: &FE00–&FE01 */
    bbc_memory_set_range_callbacks(m->mem, 0xFE00, 2,
        io_crtc_read, io_crtc_write, m);

    /* Video ULA: &FE20–&FE21 */
    bbc_memory_set_range_callbacks(m->mem, 0xFE20, 2,
        io_vidproc_read, io_vidproc_write, m);

    /* System VIA: &FE40–&FE4F (16 registers, mirrored every 16 bytes) */
    bbc_memory_set_range_callbacks(m->mem, 0xFE40, 16,
        io_sysvia_read, io_sysvia_write, m);

    /* User VIA: &FE60–&FE6F */
    bbc_memory_set_range_callbacks(m->mem, 0xFE60, 16,
        io_uservia_read, io_uservia_write, m);

    /* WD1770 FDC: &FE80–&FE87
     * The WD1770 chip select on the Acorn 1770 board ignores A2, so the four
     * chip registers at &FE80-&FE83 mirror at &FE84-&FE87 — except that
     * &FE84 is intercepted by the external drive-select latch before reaching
     * the chip.  The DFS ROM accesses:
     *   &FE80 command/status   (and mirrors at &FE84 — but &FE84 is the latch)
     *   &FE85 track reg        mirror of &FE81
     *   &FE86 sector reg       mirror of &FE82
     *   &FE87 data reg         mirror of &FE83
     */
    bbc_memory_set_range_callbacks(m->mem, 0xFE80, 8,
        io_fdc_read, io_fdc_write, m);

    /* ROMSEL: &FE30 (write selects sideways ROM slot; read returns current slot) */
    bbc_memory_set_read_callback (m->mem, 0xFE30, io_romsel_read,  m);
    bbc_memory_set_write_callback(m->mem, 0xFE30, io_romsel_write, m);

    /* MC6850 ACIA: &FE08 (status/ctrl) &FE09 (data) */
    bbc_memory_set_range_callbacks(m->mem, 0xFE08, 2,
        io_acia_read, io_acia_write, m);

    /* Serial ULA control: &FE10 (write-only).
     * bit 0 = tape motor (1=ON), bits 2:1 = TX baud, bits 4:3 = RX baud. */
    bbc_memory_set_write_callback(m->mem, 0xFE10, io_serial_ula_write, m);
}

/* ======================================================================
 * bbc_machine_load_sideways_rom
 * ====================================================================== */

void bbc_machine_load_sideways_rom(bbc_machine_t *m,
                                    const uint8_t *rom_data, uint32_t rom_size,
                                    uint8_t slot) {
    bbc_memory_load_sideways_rom(m->mem, rom_data, rom_size, slot);
}

/* ======================================================================
 * bbc_machine_reset
 * ====================================================================== */

void bbc_machine_reset(bbc_machine_t *m) {
    bbc_sysvia_reset(&m->sysvia);
    bbc_uservia_reset(&m->uservia);
    wd1770_reset(&m->fdc);
    sn76489_reset(&m->psg);
    bbc_video_reset(&m->video);

    m->irq.sysvia  = false;
    m->irq.uservia = false;
    m->irq.acia    = false;
    m->cycle_acc   = 0;

    if (m->fb_output) {
        bbc_video_set_output(&m->video, m->fb_output);
    }
    if (m->on_frame) {
        bbc_video_set_frame_callback(&m->video, m->on_frame, m->on_frame_ctx);
    }
    if (m->on_frame) {
        bbc_video_set_frame_callback(&m->video, m->on_frame, m->on_frame_ctx);
    }

    bbc_cpu_reset(m->cpu);
}

/* ======================================================================
 * bbc_machine_break
 *
 * Soft BREAK: reset CPU + peripherals without clearing RAM.
 *
 * On real BBC hardware the BREAK key pulls /RESET low for ~200 ms.
 * The OS reset handler reads $FE4E (VIA IER) to distinguish BREAK from
 * power-on, then reads $0258 to detect Ctrl-BREAK.  What matters for the
 * DFS banner is that ZP $EF survives (it is set to 'D'/0x44 by the DFS
 * workspace-init code during the first reset, so on subsequent BREAKs the
 * DFS can see it and print its banner).
 *
 * We also write $0258 bit 1 to communicate shift_held so the OS can tell
 * the DFS to autoboot (SHIFT+BREAK).
 * ====================================================================== */

void bbc_machine_break(bbc_machine_t *m, bool shift_held) {
    uint8_t *ram = bbc_memory_get_ram(m->mem);

    /* $0258: BBC OS "break key state" — bit 0 = Ctrl held, bit 1 = Shift held.
     * The DFS reads this to decide whether to autoboot. */
    if (ram) {
        ram[0x0258] = shift_held ? 0x02 : 0x00;
    }

    /* Reset peripherals (preserves RAM) */
    bbc_sysvia_reset(&m->sysvia);
    bbc_uservia_reset(&m->uservia);
    wd1770_reset(&m->fdc);
    sn76489_reset(&m->psg);
    bbc_video_reset(&m->video);

    m->irq.sysvia  = false;
    m->irq.uservia = false;
    m->irq.acia    = false;
    m->cycle_acc   = 0;
    m->crtc_acc    = 0;

    if (m->fb_output) {
        bbc_video_set_output(&m->video, m->fb_output);
    }
    if (m->on_frame) {
        bbc_video_set_frame_callback(&m->video, m->on_frame, m->on_frame_ctx);
    }

    bbc_cpu_reset(m->cpu);
}

/* ======================================================================
 * bbc_machine_step
 * ====================================================================== */

int bbc_machine_step(bbc_machine_t *m) {
    /* PC watchdog: print PC every ~2M cycles so we can see where it's spinning */
    static int32_t watchdog_cycles = 0;
    static uint16_t last_reported_pc = 0xFFFF; (void)last_reported_pc;
    watchdog_cycles--;
    if (watchdog_cycles <= 0) {
        watchdog_cycles = 2000000;
        uint16_t pc = bbc_cpu_get_pc(m->cpu);
        uint8_t *ram = bbc_memory_get_ram(m->mem);
        uint8_t acia_st = bbc_tape_read(&m->tape, 0); /* read ACIA status non-destructively */
        fprintf(stderr, "[wdog] PC=$%04X C2=$%02X EA=$%02X 0278=$%02X ACIA_st=%02X irq(sv=%d uv=%d ac=%d)\n",
                pc,
                ram ? ram[0xC2] : 0xFF,
                ram ? ram[0xEA] : 0xFF,
                ram ? ram[0x0278] : 0xFF, /* $0278 tape/serial config */
                acia_st,
                m->irq.sysvia, m->irq.uservia, m->irq.acia);
        (void)acia_st;
        last_reported_pc = pc;
    }

    int cycles = bbc_cpu_step(m->cpu);
    if (cycles <= 0) cycles = 1;

    bbc_sysvia_tick (&m->sysvia,  cycles);
    bbc_uservia_tick(&m->uservia, cycles);
    wd1770_tick     (&m->fdc,     cycles);
    bbc_tape_tick   (&m->tape,    cycles);

    /* Tick CRTC at 1 MHz (= CPU / 2).  bbc_video_tick() generates VSYNC
     * which drives the MOS frame IRQ via System VIA CA1.  Without this the
     * keyboard scanner in the MOS never runs.
     * Use a sub-cycle accumulator to approximate the 2:1 ratio. */
    m->crtc_acc += cycles;
    while (m->crtc_acc >= 2) {
        bbc_video_tick(&m->video);
        m->crtc_acc -= 2;
    }

    /* SN76489 ticking is handled by sn76489_audio_push() / sn76489_render() */

    return cycles;
}

/* ======================================================================
 * bbc_machine_mount_disk
 * ====================================================================== */

void bbc_machine_mount_disk(bbc_machine_t *m, uint8_t drive,
    int  (*read_sector) (void *, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t *, uint16_t *),
    int  (*write_sector)(void *, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, bool, const uint8_t *, uint16_t),
    void (*seek)        (void *, uint8_t, uint8_t),
    void *disk_ctx) {
    (void)drive; /* wd1770 callbacks are chip-global; drive is passed per command */
    /* Store disk I/O callbacks in the machine struct.  The FDC wrapper
     * functions (fdc_read_sector et al.) forward to these.  We do NOT
     * touch fdc.cb.user_ctx — that always points to 'm'. */
    m->disk_read_sector  = read_sector;
    m->disk_write_sector = write_sector;
    m->disk_seek         = seek;
    m->disk_ctx          = disk_ctx;
}

/* ======================================================================
 * bbc_machine_key_event
 * ====================================================================== */

void bbc_machine_key_event(bbc_machine_t *m, uint8_t row, uint8_t col, bool pressed) {
    if (row < BBC_KB_ROWS && col < BBC_KB_COLS) {
        m->keyboard.pressed[row][col] = pressed;
        /* Notify sysvia immediately so CA2 ("any key") is updated.
         * Without this, the MOS keyboard scanner never wakes up. */
        bbc_sysvia_keyboard_updated(&m->sysvia);
    }
}

/* ======================================================================
 * bbc_machine_set_video_output
 * ====================================================================== */

void bbc_machine_set_video_output(bbc_machine_t *m, bbc_video_output_t *out) {
    m->fb_output = out;
    if (out) {
        bbc_video_set_output(&m->video, out);
    }
}

/* ======================================================================
 * bbc_machine_set_frame_callback
 * ====================================================================== */

void bbc_machine_set_frame_callback(bbc_machine_t *m,
                                     void (*cb)(void *ctx), void *ctx) {
    m->on_frame     = cb;
    m->on_frame_ctx = ctx;
    bbc_video_set_frame_callback(&m->video, cb, ctx);
}

/* ======================================================================
 * System VIA callbacks
 * ====================================================================== */

/*
 * IC32 latch bit 0 (SOUND_WE) going LOW means "write to SN76489".
 * The data byte is the current value of system VIA port A.
 */
static void sv_latch_changed(void *ctx, uint8_t latch_bits) {
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    if (!(latch_bits & (1u << BBC_LATCH_SOUND_WE))) {
        /* SOUND_WE is active-LOW — write port-A data to PSG */
        uint8_t pa = bbc_sysvia_get_latch(&m->sysvia);
        /* Actually port A data is the keyboard/sound data bus */
        uint8_t pa_data = m6522_get_port_a(&m->sysvia.via);
        sn76489_write(&m->psg, pa_data);
        (void)pa;
    }
}

static void sv_sound_write(void *ctx, uint8_t data) {
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    sn76489_write(&m->psg, data);
}

static bool sv_keyboard_read(void *ctx, uint8_t row, uint8_t col) {
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    /*
     * BBC keyboard scan: MOS writes (row<<4)|col to Port A, then reads
     * bit 7 — LOW means key pressed (active low on real hardware).
     * We return true if the key at (row, col) is pressed.
     */
    if (row < BBC_KB_ROWS && col < BBC_KB_COLS)
        return m->keyboard.pressed[row][col];
    return false;
}

static void sv_irq(void *ctx, bool state) {
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    m->irq.sysvia = state;
    update_irq(m);
}

/* ======================================================================
 * User VIA callbacks
 * ====================================================================== */

static void uv_port_out(void *ctx, uint8_t port, uint8_t val, uint8_t ddr) {
    /* User port — printer / RS423 / joystick buttons; stub for now */
    (void)ctx; (void)port; (void)val; (void)ddr;
}

static uint8_t uv_port_in(void *ctx, uint8_t port) {
    /* Pull-ups → 0xFF when nothing connected */
    (void)ctx; (void)port;
    return 0xFF;
}

static void uv_irq(void *ctx, bool state) {
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    m->irq.uservia = state;
    update_irq(m);
}

/* ======================================================================
 * WD1770 callbacks
 * ====================================================================== */

static void fdc_irq(void *ctx, bool state) {
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    fprintf(stderr, "[fdc] INTRQ %s (cpu=%p)\n", state ? "assert" : "clear", (void*)m->cpu);
    if (state) {
        bbc_cpu_nmi(m->cpu);
    }
}

static void fdc_drq(void *ctx, bool state) {
    /*
     * On the Acorn 1770 FDC board, DRQ is latched in an external flip-flop
     * and becomes readable at &FE84 bit 7 (active LOW: 0 = DRQ active).
     * The DFS NMI handler reads &FE84 to decide whether the NMI was caused
     * by INTRQ (command done) or DRQ (data byte ready):
     *   &FE84 bit 7 = 0 → DRQ active  → read/write next data byte
     *   &FE84 bit 7 = 1 → INTRQ only  → command finished
     * We track DRQ state so io_fdc_read() can return the correct value.
     * DRQ also triggers NMI on the BBC 1770 interface board.
     */
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    fprintf(stderr, "[fdc] DRQ %s\n", state ? "assert" : "clear");
    m->fdc_drq_state = state;
    if (state) {
        bbc_cpu_nmi(m->cpu);
    }
}

/* ======================================================================
 * WD1770 disk I/O wrappers
 *
 * These are always installed as the FDC's read/write/seek callbacks.
 * ctx == m (bbc_machine_t *) always — safe to dereference.
 * The actual disk-image callbacks are stored in m->disk_* and called
 * with m->disk_ctx, which may be NULL if no disk is mounted.
 * ====================================================================== */

static int fdc_read_sector(void *ctx,
                            uint8_t drive, uint8_t track, uint8_t sector,
                            uint8_t side, uint8_t density,
                            uint8_t *buf, uint16_t *len)
{
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    if (!m->disk_read_sector) return -1;
    return m->disk_read_sector(m->disk_ctx,
                               drive, track, sector, side, density, buf, len);
}

static int fdc_write_sector(void *ctx,
                             uint8_t drive, uint8_t track, uint8_t sector,
                             uint8_t side, uint8_t density, bool deleted,
                             const uint8_t *buf, uint16_t len)
{
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    if (!m->disk_write_sector) return -1;
    return m->disk_write_sector(m->disk_ctx,
                                drive, track, sector, side, density, deleted,
                                buf, len);
}

static void fdc_seek(void *ctx, uint8_t drive, uint8_t track)
{
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    if (m->disk_seek)
        m->disk_seek(m->disk_ctx, drive, track);
}

/* ======================================================================
 * Video VSYNC callback
 * ====================================================================== */

static void video_vsync_cb(void *ctx, bool state) {
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    /*
     * VSYNC → System VIA CA1.  CA1 is configured for negative or positive
     * edge by PCR bit 0; bbc_sysvia_vsync() handles both edges.
     */
    bbc_sysvia_vsync(&m->sysvia, state);
}

/* ======================================================================
 * bbc_memory I/O callbacks
 * ====================================================================== */

/* CRTC MC6845 — &FE00 (address register), &FE01 (data register) */
static uint8_t io_crtc_read(uint16_t addr, void *ctx) {
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    return bbc_video_crtc_read(&m->video, (uint8_t)(addr & 1));
}
static void io_crtc_write(uint16_t addr, uint8_t val, void *ctx) {
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    bbc_video_crtc_write(&m->video, (uint8_t)(addr & 1), val);
}

/* Video ULA — &FE20 (control), &FE21 (palette) */
static uint8_t io_vidproc_read(uint16_t addr, void *ctx) {
    /* Video ULA registers are write-only; reads return 0xFF */
    (void)addr; (void)ctx;
    return 0xFF;
}
static void io_vidproc_write(uint16_t addr, uint8_t val, void *ctx) {
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    bbc_video_vidproc_write(&m->video, (uint8_t)(addr & 1), val);
}

/* System VIA IC3 — &FE40–&FE4F */
static uint8_t io_sysvia_read(uint16_t addr, void *ctx) {
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    return bbc_sysvia_read(&m->sysvia, (uint8_t)(addr & 0x0F));
}
static void io_sysvia_write(uint16_t addr, uint8_t val, void *ctx) {
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    bbc_sysvia_write(&m->sysvia, (uint8_t)(addr & 0x0F), val);
}

/* User VIA IC69 — &FE60–&FE6F */
static uint8_t io_uservia_read(uint16_t addr, void *ctx) {
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    return bbc_uservia_read(&m->uservia, (uint8_t)(addr & 0x0F));
}
static void io_uservia_write(uint16_t addr, uint8_t val, void *ctx) {
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    bbc_uservia_write(&m->uservia, (uint8_t)(addr & 0x0F), val);
}

/*
 * WD1770 FDC — Acorn 1770 board address decode (BBC Model B daughter board)
 *
 * Matches b-em FDC_ACORN decode exactly (src/wd1770.c):
 *
 *   addr & 0x04 == 0  ($FE80–$FE83): drive-select / control latch (write)
 *                                     reads return floating bus (0xFF)
 *   addr & 0x04 != 0  ($FE84–$FE87): WD1770 chip registers (read/write)
 *
 * Control latch ($FE80) bits — b-em wd1770_wctl_acorn():
 *   bit 1 = drive select  (0=drive 0, 1=drive 1)
 *   bit 2 = side select   (0=side 0, 1=side 1)
 *   bit 3 = density       (0=FM/single, 1=MFM/double)  [b-em inverts: 1→FM]
 *   bit 5 = /reset        (active LOW; 0 resets chip)
 *
 * WD1770 chip registers at $FE84–$FE87 (reg = addr & 0x03):
 *   0 = command (write) / status (read)  — reading clears INTRQ
 *   1 = track register
 *   2 = sector register
 *   3 = data register
 *
 * The NMI handler ($0D00, copied from ROM $8FD2) reads $FE84 to check
 * DRQ (bit 7 of the external latch on the real board).  We model this
 * as: reading $FE84 (reg 0, status) clears INTRQ AND returns a synthetic
 * DRQ bit so the NMI handler routes correctly.
 *
 * Note: the DFS ROM detects the chip at $9104 (STA/CMP $FE85 = track reg)
 * then reads $FE80 at $9115 expecting non-zero to confirm a disc is spinning.
 * In b-em $FE80 reads as floating bus (0xFF); AND #$03 = 0x03 ≠ 0, passes.
 * We return 0xFF for all latch reads to match this behaviour.
 */
static uint8_t io_fdc_read(uint16_t addr, void *ctx) {
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    uint16_t pc = bbc_cpu_get_pc(m->cpu);

    if (!(addr & 0x04)) {
        /* $FE80–$FE83: latch area — floating bus on real hardware.
         * Return 0xFF so the DFS detect (LDA $FE80 / AND #$03) sees
         * a non-zero value and concludes a disc is spinning. */
        fprintf(stderr, "[fdc] R $%04X(latch/float)=FF @ PC=$%04X\n", addr, pc);
        return 0xFF;
    }

    /* $FE84–$FE87: WD1770 chip registers (reg = addr & 0x03).
     *
     * $FE84 reads the WD1770 status register (same as $FE80 on the chip,
     * since A2 is ignored by the chip select).  The DFS NMI handler reads
     * $FE84 to get the status and acknowledge INTRQ.
     *
     * We clear fdc->intrq here so the INDEX-pulse edge guard allows the
     * next INDEX INTRQ to fire. */
    uint8_t reg = addr & 0x03;
    uint8_t r = wd1770_read(&m->fdc, reg);  /* reg 0 → status, clears INTRQ */
    if (reg == 0) {
        m->fdc.intrq = false;
    }
    static const char *rnames[] = {"status","track","sector","data"};
    fprintf(stderr, "[fdc] R $%04X(%s)=%02X @ PC=$%04X\n",
            addr, rnames[reg], r, pc);
    return r;
}

static void io_fdc_write(uint16_t addr, uint8_t val, void *ctx) {
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    uint16_t pc = bbc_cpu_get_pc(m->cpu);

    if (!(addr & 0x04)) {
        /* $FE80–$FE83: drive-select / control latch (b-em wd1770_wctl_acorn).
         * bit 1 = drive select  (0=drive 0, 1=drive 1)
         * bit 2 = side select   (0=side 0, 1=side 1)
         * bit 3 = density       (0=FM, 1=MFM) */
        m->fdc_latch    = val;
        uint8_t drive   = (val >> 1) & 1;
        uint8_t side    = (val >> 2) & 1;
        uint8_t density = (val >> 3) & 1;
        fprintf(stderr, "[fdc] W $%04X(latch)=%02X (drive=%d side=%d dens=%d) @ PC=$%04X\n",
                addr, val, drive, side, density, pc);
        wd1770_select(&m->fdc, drive, side, density);
        return;
    }

    /* $FE84–$FE87: WD1770 chip registers */
    uint8_t reg = addr & 0x03;
    static const char *wnames[] = {"cmd","track","sector","data"};
    fprintf(stderr, "[fdc] W $%04X(%s)=%02X @ PC=$%04X\n",
            addr, wnames[reg], val, pc);
    wd1770_write(&m->fdc, reg, val);
}

/* ROMSEL — &FE30: sideways ROM bank select */
static uint8_t io_romsel_read(uint16_t addr, void *ctx) {
    (void)addr;
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    /* On real hardware ROMSEL is write-only; reads return floating bus.
     * Return current value for debuggability. */
    return bbc_memory_get_romsel(m->mem);
}
static void io_romsel_write(uint16_t addr, uint8_t val, void *ctx) {
    (void)addr;
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    bbc_memory_set_romsel(m->mem, val & 0x0F);
}

/* ======================================================================
 * Serial ULA / ACIA — &FE08 (status/control) and &FE09 (data)
 * ====================================================================== */

static uint8_t io_acia_read(uint16_t addr, void *ctx) {
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    return bbc_tape_read(&m->tape, (uint8_t)(addr & 1));
}

static void io_acia_write(uint16_t addr, uint8_t val, void *ctx) {
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    bbc_tape_write(&m->tape, (uint8_t)(addr & 1), val);
}

/* ======================================================================
 * Serial ULA control register — &FE10 (write only)
 *
 * BBC Micro Serial ULA (IC57):
 *   bit 0   = tape motor relay  (1=ON, 0=OFF)
 *   bits 2:1 = transmit baud rate select
 *   bits 4:3 = receive baud rate select
 *   bits 7:5 = other (RS423 control, ignored here)
 * ====================================================================== */

static void io_serial_ula_write(uint16_t addr, uint8_t val, void *ctx) {
    (void)addr;
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    bool motor_on = (val & 0x01) != 0;
    fprintf(stderr, "[serial_ula] FE10 write %02X  motor=%s\n", val, motor_on ? "ON" : "OFF");
    bbc_tape_set_motor(&m->tape, motor_on);
}

/* ======================================================================
 * Tape IRQ — routes ACIA interrupt to CPU IRQ
 * ====================================================================== */

static void tape_irq(void *ctx, bool state) {
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    /* ACIA IRQ shares /IRQ line with VIA IRQs.
     * Track state so update_irq() can properly deassert the line
     * when no source is active (fixes: VIA clearing IRQ cancelled ACIA). */
    static bool last_state = false;
    if (state != last_state) {
        last_state = state;
        fprintf(stderr, "[acia] IRQ %s (sv=%d uv=%d)\n",
                state ? "ASSERT" : "clear",
                m->irq.sysvia, m->irq.uservia);
    }
    m->irq.acia = state;
    update_irq(m);
}

/* ======================================================================
 * Tape motor — called from sysvia CB2 output
 * ====================================================================== */

static void sv_motor_changed(void *ctx, bool on) {
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    bbc_tape_set_motor(&m->tape, on);
}

/* ======================================================================
 * bbc_machine_mount_tape
 * ====================================================================== */

int bbc_machine_mount_tape(bbc_machine_t *m, const char *uef_path) {
    if (!uef_path) {
        bbc_tape_free(&m->tape);
        return 0;
    }
    return bbc_tape_load_uef(&m->tape, uef_path);
}
