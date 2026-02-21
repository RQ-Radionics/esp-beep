#ifndef BBC_CPU_H
#define BBC_CPU_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BBCCPU BBCCPU;

typedef uint8_t (*BBCMemoryRead)(uint16_t addr, void *userData);
typedef void (*BBCMemoryWrite)(uint16_t addr, uint8_t value, void *userData);

BBCCPU *bbc_cpu_create(BBCMemoryRead readCallback, BBCMemoryWrite writeCallback, void *userData);
void bbc_cpu_destroy(BBBCPU *cpu);
void bbc_cpu_reset(BBBCPU *cpu);
int bbc_cpu_step(BBBCPU *cpu);
void bbc_cpu_nmi(BBBCPU *cpu);
void bbc_cpu_irq(BBBCPU *cpu);
void bbc_cpu_clear_irq(BBBCPU *cpu);

uint8_t bbc_cpu_get_a(BBBCPU *cpu);
uint8_t bbc_cpu_get_x(BBBCPU *cpu);
uint8_t bbc_cpu_get_y(BBBCPU *cpu);
uint8_t bbc_cpu_get_p(BBBCPU *cpu);
uint8_t bbc_cpu_get_sp(BBBCPU *cpu);
uint16_t bbc_cpu_get_pc(BBBCPU *cpu);

#ifdef __cplusplus
}
#endif

#endif
