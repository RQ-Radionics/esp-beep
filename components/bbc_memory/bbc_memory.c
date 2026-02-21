#include "bbc_memory.h"
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE        32768
#define ROM_BASIC_ADDR  0x8000
#define ROM_BASIC_SIZE  16384
#define ROM_OS_ADDR     0xC000
#define ROM_OS_SIZE     16384
#define IO_START        0xFC00
#define IO_SIZE         256

typedef struct {
    uint8_t ram[RAM_SIZE];
    uint8_t rom_basic[ROM_BASIC_SIZE];
    uint8_t rom_os[ROM_OS_SIZE];
    bool basic_loaded;
    bool os_loaded;
    uint8_t (*read_callback[IO_SIZE])(uint16_t, void*);
    void (*write_callback[IO_SIZE])(uint16_t, uint8_t, void*);
    void *callback_userdata[IO_SIZE];
} BBCMemoryInternal;

BBCMemory *bbc_memory_create(void) {
    BBCMemoryInternal *mem = (BBCMemoryInternal *)malloc(sizeof(BBCMemoryInternal));
    if (!mem) return NULL;
    
    memset(mem->ram, 0, RAM_SIZE);
    memset(mem->rom_basic, 0xFF, ROM_BASIC_SIZE);
    memset(mem->rom_os, 0xFF, ROM_OS_SIZE);
    mem->basic_loaded = false;
    mem->os_loaded = false;
    
    for (int i = 0; i < IO_SIZE; i++) {
        mem->read_callback[i] = NULL;
        mem->write_callback[i] = NULL;
        mem->callback_userdata[i] = NULL;
    }
    
    return (BBCMemory *)mem;
}

void bbc_memory_destroy(BBCMemory *mem) {
    if (mem) {
        free(mem);
    }
}

uint8_t bbc_memory_read(BBCMemory *mem, uint16_t addr) {
    BBCMemoryInternal *m = (BBCMemoryInternal *)mem;
    if (!m) return 0xFF;
    
    if (addr < 0x8000) {
        return m->ram[addr];
    } else if (addr < 0xC000) {
        if (m->basic_loaded) {
            return m->rom_basic[addr - 0x8000];
        }
        return 0xFF;
    } else if (addr < 0xFC00) {
        if (m->os_loaded) {
            return m->rom_os[addr - 0xC000];
        }
        return 0xFF;
    } else if (addr >= IO_START) {
        int ioIndex = addr - IO_START;
        if (ioIndex < IO_SIZE && m->read_callback[ioIndex]) {
            return m->read_callback[ioIndex](addr, m->callback_userdata[ioIndex]);
        }
        return 0xFF;
    }
    
    return 0xFF;
}

void bbc_memory_write(BBCMemory *mem, uint16_t addr, uint8_t value) {
    BBCMemoryInternal *m = (BBCMemoryInternal *)mem;
    if (!m) return;
    
    if (addr < 0x8000) {
        m->ram[addr] = value;
    } else if (addr >= IO_START && addr < 0xFE00) {
        int ioIndex = addr - IO_START;
        if (ioIndex < IO_SIZE && m->write_callback[ioIndex]) {
            m->write_callback[ioIndex](addr, value, m->callback_userdata[ioIndex]);
        }
    }
}

void bbc_memory_load_rom(BBCMemory *mem, const uint8_t *romData, uint32_t romSize, uint16_t startAddr) {
    BBCMemoryInternal *m = (BBCMemoryInternal *)mem;
    if (!m || !romData) return;
    
    if (startAddr == 0x8000 && romSize <= ROM_BASIC_SIZE) {
        memcpy(m->rom_basic, romData, romSize);
        m->basic_loaded = true;
    } else if (startAddr == 0xC000 && romSize <= ROM_OS_SIZE) {
        memcpy(m->rom_os, romData, romSize);
        m->os_loaded = true;
    }
}

void bbc_memory_set_read_callback(BBCMemory *mem, uint16_t addr, uint8_t (*callback)(uint16_t, void*), void *userData) {
    BBCMemoryInternal *m = (BBCMemoryInternal *)mem;
    if (!m || addr < IO_START || addr >= 0xFE00) return;
    
    int ioIndex = addr - IO_START;
    m->read_callback[ioIndex] = callback;
    m->callback_userdata[ioIndex] = userData;
}

void bbc_memory_set_write_callback(BBCMemory *mem, uint16_t addr, void (*callback)(uint16_t, uint8_t, void*), void *userData) {
    BBCMemoryInternal *m = (BBCMemoryInternal *)mem;
    if (!m || addr < IO_START || addr >= 0xFE00) return;
    
    int ioIndex = addr - IO_START;
    m->write_callback[ioIndex] = callback;
    m->callback_userdata[ioIndex] = userData;
}
