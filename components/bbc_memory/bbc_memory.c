/*
 * bbc_memory.c — BBC Micro Model B memory map
 *
 * Memory map:
 *   0x0000–0x7FFF  32 KB RAM
 *   0x8000–0xBFFF  Sideways ROM (16 slots × 16 KB, selected by ROMSEL at &FE30)
 *   0xC000–0xFBFF  OS ROM
 *   0xFC00–0xFEFF  I/O (Sheila) — hardware registers
 *   0xFF00–0xFFFF  OS ROM (top page — reset/interrupt vectors)
 *
 * Sideways ROM paging:
 *   &FE30 (ROMSEL) write selects which of the 16 slots is visible at &8000.
 *   Slots are 16 KB each. A slot whose rom_size is 0 reads as 0xFF.
 *   An 8 KB ROM (e.g. DFS 0.9) occupies the LOWER half of the slot
 *   (&8000–&9FFF); the upper half (&A000–&BFFF) reads as 0xFF.
 *
 * I/O callbacks:
 *   A 1 KB callback table covers &FC00–&FFFF. Reads/writes that hit an
 *   address with no registered callback fall through to the OS ROM array.
 *
 * Licence: zlib
 * Copyright (c) 2026 esp-beep project
 */

#include "bbc_memory.h"
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */
#define RAM_SIZE          32768u
#define SIDEWAYS_SLOTS    16u
#define SIDEWAYS_SLOT_SZ  16384u   /* 16 KB per slot */
#define ROM_OS_ADDR       0xC000u
#define ROM_OS_SIZE       16384u
#define IO_START          0xFC00u  /* first I/O address */
#define IO_SIZE           1024u    /* covers &FC00–&FFFF */

/* --------------------------------------------------------------------------
 * Internal struct
 * -------------------------------------------------------------------------- */
typedef struct {
    uint8_t  ram[RAM_SIZE];

    /* 16 sideways ROM slots. Stored inline (16 × 16 KB = 256 KB).
     * Only slot entries where rom_size > 0 are considered loaded. */
    uint8_t  rom_sideways[SIDEWAYS_SLOTS][SIDEWAYS_SLOT_SZ];
    uint32_t rom_sideways_size[SIDEWAYS_SLOTS];   /* bytes loaded; 0 = empty */

    /* OS ROM (&C000–&FFFF) */
    uint8_t  rom_os[ROM_OS_SIZE];
    bool     os_loaded;

    /* ROMSEL: which slot is visible at &8000 */
    uint8_t  romsel;

    /* I/O callback table for &FC00–&FFFF */
    uint8_t  (*read_callback [IO_SIZE])(uint16_t, void *);
    void     (*write_callback[IO_SIZE])(uint16_t, uint8_t, void *);
    void     *read_userdata [IO_SIZE];
    void     *write_userdata[IO_SIZE];
} BBCMemoryInternal;

/* --------------------------------------------------------------------------
 * Create / destroy
 * -------------------------------------------------------------------------- */
BBCMemory *bbc_memory_create(void) {
    BBCMemoryInternal *mem = (BBCMemoryInternal *)calloc(1, sizeof(BBCMemoryInternal));
    if (!mem) return NULL;

    memset(mem->ram, 0x00, RAM_SIZE);
    memset(mem->rom_sideways, 0xFF, sizeof(mem->rom_sideways));
    memset(mem->rom_os,       0xFF, ROM_OS_SIZE);

    /* Default ROMSEL = 15 (BASIC is conventionally in the highest slot) */
    mem->romsel = 15;

    return (BBCMemory *)mem;
}

void bbc_memory_destroy(BBCMemory *mem) {
    free(mem);
}

/* --------------------------------------------------------------------------
 * CPU-facing read
 * -------------------------------------------------------------------------- */
uint8_t bbc_memory_read(void *mem, uint16_t addr) {
    BBCMemoryInternal *m = (BBCMemoryInternal *)mem;
    if (!m) return 0xFF;

    /* RAM: &0000–&7FFF */
    if (addr < 0x8000u)
        return m->ram[addr];

    /* Sideways ROM: &8000–&BFFF */
    if (addr < 0xC000u) {
        uint16_t offset = addr - 0x8000u;
        uint8_t  slot   = m->romsel & (SIDEWAYS_SLOTS - 1);
        uint32_t sz     = m->rom_sideways_size[slot];
        if (sz > 0 && offset < sz)
            return m->rom_sideways[slot][offset];
        return 0xFF;
    }

    /* OS ROM: &C000–&FBFF */
    if (addr < IO_START)
        return m->os_loaded ? m->rom_os[addr - 0xC000u] : 0xFF;

    /* I/O / OS top page: &FC00–&FFFF */
    {
        unsigned idx = addr - IO_START;
        if (m->read_callback[idx])
            return m->read_callback[idx](addr, m->read_userdata[idx]);
        /* Fall through to OS ROM */
        return m->os_loaded ? m->rom_os[addr - 0xC000u] : 0xFF;
    }
}

/* --------------------------------------------------------------------------
 * CPU-facing write
 * -------------------------------------------------------------------------- */
void bbc_memory_write(void *mem, uint16_t addr, uint8_t value) {
    BBCMemoryInternal *m = (BBCMemoryInternal *)mem;
    if (!m) return;

    /* RAM: &0000–&7FFF */
    if (addr < 0x8000u) {
        m->ram[addr] = value;
        return;
    }

    /* Sideways ROM area: writes are ignored (ROM is read-only).
     * Sideways RAM is not implemented. */
    if (addr < IO_START)
        return;

    /* I/O: &FC00–&FFFF */
    {
        unsigned idx = addr - IO_START;
        if (m->write_callback[idx])
            m->write_callback[idx](addr, value, m->write_userdata[idx]);
        /* Unregistered I/O writes are silently dropped */
    }
}

/* --------------------------------------------------------------------------
 * Load a ROM into a specific sideways slot (0–15).
 * romSize may be 8 KB (half-slot) or 16 KB.  The data is placed at the
 * START of the slot (&8000); the remainder reads as 0xFF.
 * -------------------------------------------------------------------------- */
void bbc_memory_load_sideways_rom(BBCMemory *mem, const uint8_t *romData,
                                   uint32_t romSize, uint8_t slot) {
    BBCMemoryInternal *m = (BBCMemoryInternal *)mem;
    if (!m || !romData || romSize == 0) return;
    if (slot >= SIDEWAYS_SLOTS) return;

    uint32_t load_size = romSize;
    if (load_size > SIDEWAYS_SLOT_SZ) load_size = SIDEWAYS_SLOT_SZ;

    /* Fill slot with 0xFF first (empty read value), then copy ROM */
    memset(m->rom_sideways[slot], 0xFF, SIDEWAYS_SLOT_SZ);
    memcpy(m->rom_sideways[slot], romData, load_size);
    m->rom_sideways_size[slot] = load_size;
}

/* --------------------------------------------------------------------------
 * ROM loading — legacy entry point.
 *
 *   startAddr == 0x8000: load into slot 15 (BASIC, backwards compat).
 *   startAddr == 0xC000: load into the OS ROM area.
 *
 * For explicit slot selection use bbc_memory_load_sideways_rom().
 * -------------------------------------------------------------------------- */
void bbc_memory_load_rom(BBCMemory *mem, const uint8_t *romData,
                          uint32_t romSize, uint16_t startAddr) {
    BBCMemoryInternal *m = (BBCMemoryInternal *)mem;
    if (!m || !romData || romSize == 0) return;

    if (startAddr == 0x8000u) {
        bbc_memory_load_sideways_rom(mem, romData, romSize, 15);
    } else if (startAddr == 0xC000u && romSize <= ROM_OS_SIZE) {
        memcpy(m->rom_os, romData, romSize);
        m->os_loaded = true;
    }
}

/* --------------------------------------------------------------------------
 * ROMSEL — set which sideways slot is visible at &8000.
 * Called by the ROMSEL I/O callback (&FE30 write).
 * -------------------------------------------------------------------------- */
void bbc_memory_set_romsel(BBCMemory *mem, uint8_t slot) {
    BBCMemoryInternal *m = (BBCMemoryInternal *)mem;
    if (m) m->romsel = slot & (SIDEWAYS_SLOTS - 1);
}

uint8_t bbc_memory_get_romsel(const BBCMemory *mem) {
    const BBCMemoryInternal *m = (const BBCMemoryInternal *)mem;
    return m ? m->romsel : 0;
}

/* --------------------------------------------------------------------------
 * I/O callback registration
 * -------------------------------------------------------------------------- */
void bbc_memory_set_read_callback(BBCMemory *mem, uint16_t addr,
    uint8_t (*callback)(uint16_t, void *), void *userData) {
    BBCMemoryInternal *m = (BBCMemoryInternal *)mem;
    if (!m || addr < IO_START) return;
    unsigned idx = addr - IO_START;
    if (idx >= IO_SIZE) return;
    m->read_callback[idx]  = callback;
    m->read_userdata[idx]  = userData;
}

void bbc_memory_set_write_callback(BBCMemory *mem, uint16_t addr,
    void (*callback)(uint16_t, uint8_t, void *), void *userData) {
    BBCMemoryInternal *m = (BBCMemoryInternal *)mem;
    if (!m || addr < IO_START) return;
    unsigned idx = addr - IO_START;
    if (idx >= IO_SIZE) return;
    m->write_callback[idx] = callback;
    m->write_userdata[idx] = userData;
}

void bbc_memory_set_range_callbacks(BBCMemory *mem,
    uint16_t base, uint16_t len,
    uint8_t (*rd)(uint16_t, void *),
    void    (*wr)(uint16_t, uint8_t, void *),
    void    *userData) {
    BBCMemoryInternal *m = (BBCMemoryInternal *)mem;
    if (!m || base < IO_START) return;
    for (uint16_t a = base; a < (uint16_t)(base + len); a++) {
        unsigned idx = a - IO_START;
        if (idx >= IO_SIZE) break;
        m->read_callback [idx] = rd;
        m->write_callback[idx] = wr;
        m->read_userdata [idx] = userData;
        m->write_userdata[idx] = userData;
    }
}

/* --------------------------------------------------------------------------
 * Direct RAM access (for video subsystem DMA)
 * -------------------------------------------------------------------------- */
uint8_t *bbc_memory_get_ram(BBCMemory *mem) {
    BBCMemoryInternal *m = (BBCMemoryInternal *)mem;
    return m ? m->ram : NULL;
}
