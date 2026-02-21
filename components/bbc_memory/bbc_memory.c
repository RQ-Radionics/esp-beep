#include "bbc_memory.h"
#include <stdlib.h>
#include <string.h>

/*
 * BBC Micro Model B memory map:
 *   0x0000–0x7FFF  32 KB RAM
 *   0x8000–0xBFFF  Sideways ROM (BASIC, paged ROMs)
 *   0xC000–0xFBFF  OS ROM
 *   0xFC00–0xFEFF  I/O (Sheila) — hardware registers
 *     0xFC00–0xFCFF  Reserved / TUBE
 *     0xFD00–0xFDFF  Reserved
 *     0xFE00–0xFE0F  CRTC MC6845          (&FE00/&FE01)
 *     0xFE20–0xFE2F  Video ULA            (&FE20/&FE21)
 *     0xFE40–0xFE4F  System VIA (MOS 6522 IC3)
 *     0xFE60–0xFE6F  User VIA  (MOS 6522 IC69)
 *     0xFE80–0xFE8F  WD1770 FDC           (&FE80–&FE83, &FE84 drive select)
 *     0xFEC0–0xFECF  ADC (uPD7002)
 *     0xFEE0–0xFEEF  Tube ULA (optional)
 *   0xFF00–0xFFFF  OS ROM (top page — reset/interrupt vectors)
 *
 * We use a 1 KB callback table covering 0xFC00–0xFFFF so all of Sheila
 * plus the OS top-page vectors are reachable.  The OS ROM read falls
 * through to the ROM array for addresses where no callback is registered.
 */

#define RAM_SIZE        32768u
#define ROM_BASIC_ADDR  0x8000u
#define ROM_BASIC_SIZE  16384u
#define ROM_OS_ADDR     0xC000u
#define ROM_OS_SIZE     16384u
#define IO_START        0xFC00u   /* first I/O address */
#define IO_SIZE         1024u     /* covers 0xFC00–0xFFFF */

typedef struct {
    uint8_t  ram[RAM_SIZE];
    uint8_t  rom_basic[ROM_BASIC_SIZE];
    uint8_t  rom_os[ROM_OS_SIZE];
    bool     basic_loaded;
    bool     os_loaded;
    uint8_t  (*read_callback [IO_SIZE])(uint16_t, void *);
    void     (*write_callback[IO_SIZE])(uint16_t, uint8_t, void *);
    void     *read_userdata [IO_SIZE];
    void     *write_userdata[IO_SIZE];
} BBCMemoryInternal;

BBCMemory *bbc_memory_create(void) {
    BBCMemoryInternal *mem = (BBCMemoryInternal *)malloc(sizeof(BBCMemoryInternal));
    if (!mem) return NULL;

    memset(mem->ram,       0x00, RAM_SIZE);
    memset(mem->rom_basic, 0xFF, ROM_BASIC_SIZE);
    memset(mem->rom_os,    0xFF, ROM_OS_SIZE);
    mem->basic_loaded = false;
    mem->os_loaded    = false;

    for (unsigned i = 0; i < IO_SIZE; i++) {
        mem->read_callback [i] = NULL;
        mem->write_callback[i] = NULL;
        mem->read_userdata [i] = NULL;
        mem->write_userdata[i] = NULL;
    }

    return (BBCMemory *)mem;
}

void bbc_memory_destroy(BBCMemory *mem) {
    free(mem);
}

uint8_t bbc_memory_read(BBCMemory *mem, uint16_t addr) {
    BBCMemoryInternal *m = (BBCMemoryInternal *)mem;
    if (!m) return 0xFF;

    if (addr < 0x8000u) {
        return m->ram[addr];
    }
    if (addr < 0xC000u) {
        return m->basic_loaded ? m->rom_basic[addr - 0x8000u] : 0xFF;
    }
    if (addr < IO_START) {
        /* OS ROM 0xC000–0xFBFF */
        return m->os_loaded ? m->rom_os[addr - 0xC000u] : 0xFF;
    }
    /* I/O / top of OS ROM: 0xFC00–0xFFFF */
    {
        unsigned idx = addr - IO_START;
        if (m->read_callback[idx]) {
            return m->read_callback[idx](addr, m->read_userdata[idx]);
        }
        /* Fall through to OS ROM for 0xFC00–0xFFFF */
        return m->os_loaded ? m->rom_os[addr - 0xC000u] : 0xFF;
    }
}

void bbc_memory_write(BBCMemory *mem, uint16_t addr, uint8_t value) {
    BBCMemoryInternal *m = (BBCMemoryInternal *)mem;
    if (!m) return;

    if (addr < 0x8000u) {
        m->ram[addr] = value;
        return;
    }
    if (addr >= IO_START) {
        unsigned idx = addr - IO_START;
        if (m->write_callback[idx]) {
            m->write_callback[idx](addr, value, m->write_userdata[idx]);
        }
    }
    /* writes to ROM / unmapped are silently dropped */
}

void bbc_memory_load_rom(BBCMemory *mem, const uint8_t *romData,
                          uint32_t romSize, uint16_t startAddr) {
    BBCMemoryInternal *m = (BBCMemoryInternal *)mem;
    if (!m || !romData) return;

    if (startAddr == ROM_BASIC_ADDR && romSize <= ROM_BASIC_SIZE) {
        memcpy(m->rom_basic, romData, romSize);
        m->basic_loaded = true;
    } else if (startAddr == ROM_OS_ADDR && romSize <= ROM_OS_SIZE) {
        memcpy(m->rom_os, romData, romSize);
        m->os_loaded = true;
    }
}

/*
 * Register a read callback for a single address in 0xFC00–0xFFFF.
 * The same userData pointer is used for both read and write unless
 * set individually via the two separate calls.
 */
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

/*
 * Convenience: register the same read+write handler pair for a contiguous
 * range of addresses [base, base+len).  All entries share the same userData.
 */
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

/*
 * Direct access to the RAM array — used by the video subsystem to read
 * screen memory without going through the callback machinery.
 */
uint8_t *bbc_memory_get_ram(BBCMemory *mem) {
    BBCMemoryInternal *m = (BBCMemoryInternal *)mem;
    return m ? m->ram : NULL;
}
