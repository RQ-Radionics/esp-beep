#include "bbc_video.h"
#include <stdlib.h>
#include <string.h>

#define CRTC_ADDR_REG    0
#define CRTC_DATA_REG    1

typedef struct {
    uint8_t crtc_regs[18];
    uint8_t crtc_addr_register;
    uint8_t ula_control;
    uint8_t ula_palette[16];
    uint8_t current_mode;
    uint8_t *ram;
    uint32_t ram_size;
    uint32_t hsync_count;
    uint32_t vsync_count;
    bool vsync_active;
    uint32_t screen_address;
    uint32_t cursor_address;
    BBCVideoRefreshCallback refresh_callback;
    void *callback_userdata;
} BBCVideoInternal;

const uint8_t BBC_MODE_COLORS[] = {
    2, 4, 2, 4, 2, 16, 4, 1
};

const uint32_t BBC_MODE_WIDTHS[] = {
    640, 640, 640, 640, 640, 640, 640, 640
};

const uint32_t BBC_MODE_HEIGHTS[] = {
    256, 256, 256, 200, 200, 200, 250, 250
};

BBCVideo *bbc_video_create(void) {
    BBCVideoInternal *video = (BBCVideoInternal *)malloc(sizeof(BBCVideoInternal));
    if (!video) return NULL;
    
    memset(video->crtc_regs, 0, sizeof(video->crtc_regs));
    video->crtc_addr_register = 0;
    video->ula_control = 0;
    video->current_mode = 7;
    video->ram = NULL;
    video->ram_size = 0;
    video->hsync_count = 0;
    video->vsync_count = 0;
    video->vsync_active = false;
    video->screen_address = 0x4000;
    video->cursor_address = 0;
    video->refresh_callback = NULL;
    video->callback_userdata = NULL;
    
    for (int i = 0; i < 16; i++) {
        video->ula_palette[i] = i & 0x07;
    }
    
    return (BBCVideo *)video;
}

void bbc_video_destroy(BBCVideo *video) {
    if (video) {
        free(video);
    }
}

void bbc_video_set_memory(BBCVideo *video, uint8_t *ram, uint32_t ramSize) {
    BBCVideoInternal *v = (BBCVideoInternal *)video;
    if (!v) return;
    
    v->ram = ram;
    v->ram_size = ramSize;
}

void bbc_video_update_mode(BBCVideo *video, uint8_t mode) {
    BBCVideoInternal *v = (BBCVideoInternal *)video;
    if (!v) return;
    
    v->current_mode = mode & 0x07;
}

void bbc_video_write_crtc(BBCVideo *video, uint8_t addr, uint8_t value) {
    BBCVideoInternal *v = (BBCVideoInternal *)video;
    if (!v) return;
    
    if (addr == CRTC_ADDR_REG) {
        v->crtc_addr_register = value & 0x1F;
    } else if (addr == CRTC_DATA_REG) {
        v->crtc_regs[v->crtc_addr_register] = value;
        
        if (v->crtc_addr_register == 12) {
            v->screen_address = (v->screen_address & 0x00FF) | ((uint32_t)value << 8);
        } else if (v->crtc_addr_register == 13) {
            v->screen_address = (v->screen_address & 0xFF00) | ((uint32_t)value);
        }
    }
}

void bbc_video_write_ula(BBCVideo *video, uint8_t value) {
    BBCVideoInternal *v = (BBCVideoInternal *)video;
    if (!v) return;
    
    v->ula_control = value;
    
    for (int i = 0; i < 16; i++) {
        v->ula_palette[i] = (value >> (i < 8 ? 0 : 4)) & 0x0F;
    }
}

void bbc_video_tick(BBCVideo *video, uint32_t cycles) {
    BBCVideoInternal *v = (BBCVideoInternal *)video;
    if (!v || !v->ram) return;
    
    for (uint32_t i = 0; i < cycles; i++) {
        v->hsync_count++;
        
        uint32_t chars_per_line = v->crtc_regs[1] + 1;
        uint32_t total_chars = v->crtc_regs[0] + 1;
        
        if (v->hsync_count >= total_chars) {
            v->hsync_count = 0;
            v->vsync_count++;
            
            uint32_t total_lines = v->crtc_regs[4] + 1;
            if (v->vsync_count >= total_lines) {
                v->vsync_count = 0;
                v->vsync_active = false;
                
                if (v->refresh_callback) {
                    v->refresh_callback(video, v->callback_userdata);
                }
            }
        }
    }
}

void bbc_video_set_refresh_callback(BBCVideo *video, BBCVideoRefreshCallback callback, void *userData) {
    BBCVideoInternal *v = (BBCVideoInternal *)video;
    if (!v) return;
    
    v->refresh_callback = callback;
    v->callback_userdata = userData;
}

uint8_t bbc_video_get_ula_palette(BBCVideo *video, uint8_t colorIndex) {
    BBCVideoInternal *v = (BBCVideoInternal *)video;
    if (!v || colorIndex >= 16) return 0;
    
    return v->ula_palette[colorIndex];
}
