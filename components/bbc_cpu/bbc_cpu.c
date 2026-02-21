#include "bbc_cpu.h"
#include "vrEmu6502.h"
#include <stdlib.h>
#include <stdio.h>

/*
 * vrEmu6502 uses global (non-context) memory callbacks.
 * We support a single BBCCPU instance at a time via a static pointer.
 * This is sufficient for single-machine emulation on ESP32.
 */
static BBCCPU *s_active_cpu = NULL;

struct BBCCPU {
    VrEmu6502      *cpu;
    BBCMemoryRead   readCallback;
    BBCMemoryWrite  writeCallback;
    void           *userData;
};

/* vrEmu6502 global read/write callbacks */
static uint8_t cpu_mem_read(uint16_t addr, bool isDbg) {
    if (s_active_cpu && s_active_cpu->readCallback && !isDbg) {
        return s_active_cpu->readCallback(addr, s_active_cpu->userData);
    }
    if (s_active_cpu && s_active_cpu->readCallback && isDbg) {
        /* Debug reads should not cause side-effects; still forward but
         * the memory system's read is side-effect-free for ROM/RAM.     */
        return s_active_cpu->readCallback(addr, s_active_cpu->userData);
    }
    return 0xFF;
}

static void cpu_mem_write(uint16_t addr, uint8_t value) {
    if (s_active_cpu && s_active_cpu->writeCallback) {
        s_active_cpu->writeCallback(addr, value, s_active_cpu->userData);
    }
}

BBCCPU *bbc_cpu_create(BBCMemoryRead readCallback, BBCMemoryWrite writeCallback, void *userData) {
    BBCCPU *bbc = (BBCCPU *)malloc(sizeof(BBCCPU));
    if (!bbc) return NULL;

    bbc->readCallback  = readCallback;
    bbc->writeCallback = writeCallback;
    bbc->userData      = userData;

    /* Register as active CPU before creating vrEmu6502 */
    s_active_cpu = bbc;

    bbc->cpu = vrEmu6502New(CPU_6502, cpu_mem_read, cpu_mem_write);
    if (!bbc->cpu) {
        free(bbc);
        s_active_cpu = NULL;
        return NULL;
    }

    return bbc;
}

void bbc_cpu_destroy(BBCCPU *cpu) {
    if (cpu) {
        if (cpu->cpu) {
            vrEmu6502Destroy(cpu->cpu);
        }
        if (s_active_cpu == cpu) {
            s_active_cpu = NULL;
        }
        free(cpu);
    }
}

void bbc_cpu_reset(BBCCPU *cpu) {
    if (cpu && cpu->cpu) {
        s_active_cpu = cpu;
        vrEmu6502Reset(cpu->cpu);
    }
}

/*
 * vrEmu6502InstCycle() executes one full instruction and returns the
 * number of clock cycles it consumed.
 */
int bbc_cpu_step(BBCCPU *cpu) {
    if (cpu && cpu->cpu) {
        s_active_cpu = cpu;
        return (int)vrEmu6502InstCycle(cpu->cpu);
    }
    return 0;
}

void bbc_cpu_nmi(BBCCPU *cpu) {
    if (cpu && cpu->cpu) {
        *vrEmu6502Nmi(cpu->cpu) = IntRequested;
    }
}

void bbc_cpu_irq(BBCCPU *cpu) {
    if (cpu && cpu->cpu) {
        *vrEmu6502Int(cpu->cpu) = IntRequested;
    }
}

void bbc_cpu_clear_irq(BBCCPU *cpu) {
    if (cpu && cpu->cpu) {
        *vrEmu6502Int(cpu->cpu) = IntCleared;
    }
}

uint8_t bbc_cpu_get_a(BBCCPU *cpu) {
    if (cpu && cpu->cpu) return vrEmu6502GetAcc(cpu->cpu);
    return 0;
}

uint8_t bbc_cpu_get_x(BBCCPU *cpu) {
    if (cpu && cpu->cpu) return vrEmu6502GetX(cpu->cpu);
    return 0;
}

uint8_t bbc_cpu_get_y(BBCCPU *cpu) {
    if (cpu && cpu->cpu) return vrEmu6502GetY(cpu->cpu);
    return 0;
}

uint8_t bbc_cpu_get_p(BBCCPU *cpu) {
    if (cpu && cpu->cpu) return vrEmu6502GetStatus(cpu->cpu);
    return 0;
}

uint8_t bbc_cpu_get_sp(BBCCPU *cpu) {
    if (cpu && cpu->cpu) return vrEmu6502GetStackPointer(cpu->cpu);
    return 0;
}

uint16_t bbc_cpu_get_pc(BBCCPU *cpu) {
    if (cpu && cpu->cpu) return vrEmu6502GetPC(cpu->cpu);
    return 0;
}
