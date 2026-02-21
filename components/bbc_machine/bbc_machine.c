/*
 * bbc_machine.c — BBC Micro Model B top-level integration
 *
 * Licence: zlib
 * Copyright (c) 2026 esp-beep project
 */

#include "bbc_machine.h"
#include <string.h>

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

/* ======================================================================
 * IRQ helpers
 * ====================================================================== */

/* Recalculate the CPU /IRQ line from all sources */
static void update_irq(bbc_machine_t *m) {
    if (m->irq.sysvia || m->irq.uservia) {
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
    {
        wd1770_callbacks_t cb = {
            .read_sector  = NULL,   /* mounted later via bbc_machine_mount_disk */
            .write_sector = NULL,
            .seek         = NULL,
            .irq          = fdc_irq,
            .drq          = fdc_drq,
            .user_ctx     = m,
        };
        wd1770_init(&m->fdc, &cb);
    }

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

    /* WD1770 FDC: &FE80–&FE84 (&FE84 = drive/side/density select) */
    bbc_memory_set_range_callbacks(m->mem, 0xFE80, 5,
        io_fdc_read, io_fdc_write, m);
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
    m->cycle_acc   = 0;

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
    int cycles = bbc_cpu_step(m->cpu);
    if (cycles <= 0) cycles = 1;

    bbc_sysvia_tick (&m->sysvia,  cycles);
    bbc_uservia_tick(&m->uservia, cycles);
    wd1770_tick     (&m->fdc,     cycles);

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
    m->fdc.cb.read_sector  = read_sector;
    m->fdc.cb.write_sector = write_sector;
    m->fdc.cb.seek         = seek;
    m->fdc.cb.user_ctx     = disk_ctx;
}

/* ======================================================================
 * bbc_machine_key_event
 * ====================================================================== */

void bbc_machine_key_event(bbc_machine_t *m, uint8_t row, uint8_t col, bool pressed) {
    if (row < BBC_KB_ROWS && col < BBC_KB_COLS) {
        m->keyboard.pressed[row][col] = pressed;
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
    if (state) {
        bbc_cpu_nmi(m->cpu);
    }
    /* NMI is edge-triggered — no clear needed */
}

static void fdc_drq(void *ctx, bool state) {
    /* DRQ on the BBC is wired to User VIA CB1 for some interfaces, but
     * for the standard Acorn 1770 interface DRQ is handled via a latch
     * at &FE84.  We leave it as a stub for now. */
    (void)ctx; (void)state;
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
 * WD1770 FDC — &FE80–&FE83 (chip registers), &FE84 (drive select latch)
 *
 * Drive select latch (&FE84) bits (Acorn 8271 compatible interface):
 *   bit 0  drive 0 select (active HIGH)
 *   bit 1  drive 1 select (active HIGH)
 *   bit 2  side select (0=side 0, 1=side 1)
 *   bit 3  density  (0=FM/single, 1=MFM/double)
 */
static uint8_t io_fdc_read(uint16_t addr, void *ctx) {
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    uint8_t reg = (uint8_t)(addr - 0xFE80);
    if (reg <= 3) {
        return wd1770_read(&m->fdc, reg);
    }
    return 0xFF;
}
static void io_fdc_write(uint16_t addr, uint8_t val, void *ctx) {
    bbc_machine_t *m = (bbc_machine_t *)ctx;
    uint8_t reg = (uint8_t)(addr - 0xFE80);
    if (reg <= 3) {
        wd1770_write(&m->fdc, reg, val);
    } else if (reg == 4) {
        /* Drive select latch */
        uint8_t drive   = (val & 0x01) ? 0 : 1;   /* bit 0 = drive 0 */
        uint8_t side    = (val >> 2) & 1;
        uint8_t density = (val >> 3) & 1;
        wd1770_select(&m->fdc, drive, side, density);
    }
}
