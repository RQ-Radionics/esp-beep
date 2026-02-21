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
void bbc_cpu_destroy(BBCCPU *cpu);
void bbc_cpu_reset(BBCCPU *cpu);
int bbc_cpu_step(BBCCPU *cpu);
void bbc_cpu_nmi(BBCCPU *cpu);
void bbc_cpu_irq(BBCCPU *cpu);
void bbc_cpu_clear_irq(BBCCPU *cpu);

uint8_t bbc_cpu_get_a(BBCCPU *cpu);
uint8_t bbc_cpu_get_x(BBCCPU *cpu);
uint8_t bbc_cpu_get_y(BBCCPU *cpu);
uint8_t bbc_cpu_get_p(BBCCPU *cpu);
uint8_t bbc_cpu_get_sp(BBCCPU *cpu);
uint16_t bbc_cpu_get_pc(BBCCPU *cpu);

#ifdef __cplusplus
}
#endif

#endif
