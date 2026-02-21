#ifndef BBC_VIDEO_H
#define BBC_VIDEO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BBCVideo BBCVideo;

typedef void (*BBCVideoRefreshCallback)(BBCVideo *video, void *userData);

BBCVideo *bbc_video_create(void);
void bbc_video_destroy(BBCVideo *video);

void bbc_video_set_memory(BBCVideo *video, uint8_t *ram, uint32_t ramSize);

void bbc_video_update_mode(BBCVideo *video, uint8_t mode);
void bbc_video_write_crtc(BBCVideo *video, uint8_t addr, uint8_t value);
void bbc_video_write_ula(BBCVideo *video, uint8_t value);

void bbc_video_tick(BBCVideo *video, uint32_t cycles);

void bbc_video_set_refresh_callback(BBCVideo *video, BBCVideoRefreshCallback callback, void *userData);

uint8_t bbc_video_get_ula_palette(BBCVideo *video, uint8_t colorIndex);

#ifdef __cplusplus
}
#endif

#endif
