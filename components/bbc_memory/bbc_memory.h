#ifndef BBC_MEMORY_H
#define BBC_MEMORY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BBCMemory BBCMemory;

BBCMemory *bbc_memory_create(void);
void bbc_memory_destroy(BBCMemory *mem);

uint8_t bbc_memory_read(BBCMemory *mem, uint16_t addr);
void bbc_memory_write(BBCMemory *mem, uint16_t addr, uint8_t value);

void bbc_memory_load_rom(BBCMemory *mem, const uint8_t *romData, uint32_t romSize, uint16_t startAddr);

void bbc_memory_set_read_callback(BBCMemory *mem, uint16_t addr, uint8_t (*callback)(uint16_t, void*), void *userData);
void bbc_memory_set_write_callback(BBCMemory *mem, uint16_t addr, void (*callback)(uint16_t, uint8_t, void*), void *userData);

#ifdef __cplusplus
}
#endif

#endif
