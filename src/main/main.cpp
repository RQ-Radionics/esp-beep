#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fabgl.h"
#include "esp_system.h"
#include "bbc_cpu.h"
#include "bbc_memory.h"
#include "roms.h"

static fabgl::VGA16Controller *VGA16Controller;
static BBCCPU *cpu = nullptr;
static BBCMemory *memory = nullptr;

static uint8_t memoryRead(uint16_t addr, void *userData) {
    return bbc_memory_read(memory, addr);
}

static void memoryWrite(uint16_t addr, uint8_t value, void *userData) {
    bbc_memory_write(memory, addr, value);
}

void vgaTask(void *arg) {
    VGA16Controller = new fabgl::VGA16Controller();
    VGA16Controller->init();
    
    auto *canvas = VGA16Controller->getCanvas();
    canvas->setBrushColor(Color::Black);
    canvas->clear();
    canvas->setTextColor(Color::BrightGreen);
    canvas->drawText(10, 10, "ESP32 BBC Micro Emulator");
    canvas->drawText(10, 30, "Memory Initialized");
    
    while (true) {
        if (cpu) {
            bbc_cpu_step(cpu);
        }
        vTaskDelay(1);
    }
}

void app_main() {
    printf("BBC Micro Emulator for ESP32\n");
    printf("Starting...\n");
    
    memory = bbc_memory_create();
    if (!memory) {
        printf("Failed to create memory\n");
        return;
    }
    
    uint32_t osSize = os12_rom_end - os12_rom;
    uint32_t basicSize = basic2_rom_end - basic2_rom;
    
    printf("Loading OS ROM: %lu bytes\n", osSize);
    printf("Loading BASIC ROM: %lu bytes\n", basicSize);
    
    bbc_memory_load_rom(memory, os12_rom, osSize, 0xC000);
    bbc_memory_load_rom(memory, basic2_rom, basicSize, 0x8000);
    
    cpu = bbc_cpu_create(memoryRead, memoryWrite, nullptr);
    if (cpu) {
        printf("CPU created successfully\n");
        bbc_cpu_reset(cpu);
    } else {
        printf("Failed to create CPU\n");
    }
    
    xTaskCreatePinnedToCore(vgaTask, "VGA", 4096, NULL, 5, NULL, 0);
}
