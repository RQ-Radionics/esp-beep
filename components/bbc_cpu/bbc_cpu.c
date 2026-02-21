#include "bbc_cpu.h"
#include "vrEmu6502.h"
#include <stdlib.h>
#include <stdio.h>

struct BBCCPU {
    VrEmu6502 *cpu;
    BBCMemoryRead readCallback;
    BBCMemoryWrite writeCallback;
    void *userData;
};

static uint8_t memoryReadWrapper(uint16_t addr, bool isDbg, void *userData) {
    BBCCPU *bbc = (BBCCPU *)userData;
    if (bbc->readCallback) {
        return bbc->readCallback(addr, bbc->userData);
    }
    return 0xFF;
}

static void memoryWriteWrapper(uint16_t addr, uint8_t value, bool isDbg, void *userData) {
    BBCCPU *bbc = (BBCCPU *)userData;
    if (bbc->writeCallback) {
        bbc->writeCallback(addr, value, bbc->userData);
    }
}

BBCCPU *bbc_cpu_create(BBCMemoryRead readCallback, BBCMemoryWrite writeCallback, void *userData) {
    BBCCPU *bbc = (BBCCPU *)malloc(sizeof(BBCCPU));
    if (!bbc) return NULL;
    
    bbc->readCallback = readCallback;
    bbc->writeCallback = writeCallback;
    bbc->userData = userData;
    
    bbc->cpu = vrEmu6502New(CPU_6502, memoryReadWrapper, memoryWriteWrapper, bbc);
    if (!bbc->cpu) {
        free(bbc);
        return NULL;
    }
    
    return bbc;
}

void bbc_cpu_destroy(BBCCPU *cpu) {
    if (cpu) {
        if (cpu->cpu) {
            vrEmu6502Destroy(cpu->cpu);
        }
        free(cpu);
    }
}

void bbc_cpu_reset(BBCCPU *cpu) {
    if (cpu && cpu->cpu) {
        vrEmu6502Reset(cpu->cpu);
    }
}

int bbc_cpu_step(BBCCPU *cpu) {
    if (cpu && cpu->cpu) {
        return vrEmu6502Step(cpu->cpu);
    }
    return 0;
}

void bbc_cpu_nmi(BBCCPU *cpu) {
    if (cpu && cpu->cpu) {
        vrEmu6502NMI(cpu->cpu);
    }
}

void bbc_cpu_irq(BBCCPU *cpu) {
    if (cpu && cpu->cpu) {
        vrEmu6502Interrupt(cpu->cpu);
    }
}

void bbc_cpu_clear_irq(BBCCPU *cpu) {
    if (cpu && cpu->cpu) {
        vrEmu6502ClearInterrupt(cpu->cpu);
    }
}

uint8_t bbc_cpu_get_a(BBCCPU *cpu) {
    if (cpu && cpu->cpu) {
        return vrEmu6502GetRegA(cpu->cpu);
    }
    return 0;
}

uint8_t bbc_cpu_get_x(BBCCPU *cpu) {
    if (cpu && cpu->cpu) {
        return vrEmu6502GetRegX(cpu->cpu);
    }
    return 0;
}

uint8_t bbc_cpu_get_y(BBCCPU *cpu) {
    if (cpu && cpu->cpu) {
        return vrEmu6502GetRegY(cpu->cpu);
    }
    return 0;
}

uint8_t bbc_cpu_get_p(BBCCPU *cpu) {
    if (cpu && cpu->cpu) {
        return vrEmu6502GetStatus(cpu->cpu);
    }
    return 0;
}

uint8_t bbc_cpu_get_sp(BBCCPU *cpu) {
    if (cpu && cpu->cpu) {
        return vrEmu6502GetRegS(cpu->cpu);
    }
    return 0;
}

uint16_t bbc_cpu_get_pc(BBCCPU *cpu) {
    if (cpu && cpu->cpu) {
        return vrEmu6502GetPC(cpu->cpu);
    }
    return 0;
}
