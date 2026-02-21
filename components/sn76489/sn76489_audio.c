/*
 * sn76489_audio.c — I2S audio bridge for SN76489 PSG on ESP32
 *
 * Drives an I2S DAC (or the internal DAC via I2S0) with PCM samples
 * rendered by sn76489_render().  Uses the ESP-IDF v5.x I2S driver
 * (i2s_std API).
 *
 * Usage:
 *   1. Call sn76489_audio_init() once after sn76489_init().
 *   2. Call sn76489_audio_task() from a FreeRTOS task (loops forever,
 *      fills a DMA buffer, calls sn76489_write() for pending bytes).
 *   3. Call sn76489_audio_deinit() on shutdown.
 *
 * Licence: GPL-2.0
 * Copyright (c) 2026 esp-beep project
 */

#ifdef ESP_PLATFORM

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "sn76489.h"

static const char *TAG = "sn76489_audio";

/* --------------------------------------------------------------------------
 * Configuration — override in sdkconfig / Kconfig if needed
 * -------------------------------------------------------------------------- */
#ifndef SN76489_AUDIO_SAMPLE_RATE
#  define SN76489_AUDIO_SAMPLE_RATE   22050
#endif

#ifndef SN76489_AUDIO_DMA_BUF_COUNT
#  define SN76489_AUDIO_DMA_BUF_COUNT  4
#endif

#ifndef SN76489_AUDIO_DMA_BUF_LEN
#  define SN76489_AUDIO_DMA_BUF_LEN    512   /* samples per DMA buffer    */
#endif

/* I2S pin defaults — override in your board header */
#ifndef SN76489_I2S_BCK_PIN
#  define SN76489_I2S_BCK_PIN     26
#endif
#ifndef SN76489_I2S_WS_PIN
#  define SN76489_I2S_WS_PIN      25
#endif
#ifndef SN76489_I2S_DATA_PIN
#  define SN76489_I2S_DATA_PIN    22
#endif
#ifndef SN76489_I2S_PORT
#  define SN76489_I2S_PORT        I2S_NUM_0
#endif

/* --------------------------------------------------------------------------
 * Module state
 * -------------------------------------------------------------------------- */
static i2s_chan_handle_t s_i2s_tx_chan = NULL;

/* DMA transmit buffer: stereo 16-bit (L=R=mono sample) */
static int16_t s_dma_buf[SN76489_AUDIO_DMA_BUF_LEN * 2];

/* --------------------------------------------------------------------------
 * Public: initialise I2S peripheral
 * -------------------------------------------------------------------------- */
esp_err_t sn76489_audio_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        SN76489_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = SN76489_AUDIO_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = SN76489_AUDIO_DMA_BUF_LEN;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_i2s_tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SN76489_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .bclk = SN76489_I2S_BCK_PIN,
            .ws   = SN76489_I2S_WS_PIN,
            .dout = SN76489_I2S_DATA_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_i2s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s",
                 esp_err_to_name(err));
        i2s_del_channel(s_i2s_tx_chan);
        s_i2s_tx_chan = NULL;
        return err;
    }

    err = i2s_channel_enable(s_i2s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_i2s_tx_chan);
        s_i2s_tx_chan = NULL;
        return err;
    }

    ESP_LOGI(TAG, "I2S init OK: %d Hz, port=%d bck=%d ws=%d dout=%d",
             SN76489_AUDIO_SAMPLE_RATE, SN76489_I2S_PORT,
             SN76489_I2S_BCK_PIN, SN76489_I2S_WS_PIN, SN76489_I2S_DATA_PIN);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Public: render one DMA buffer and send to I2S
 *
 * Call this from a dedicated FreeRTOS task.  The i2s_channel_write() call
 * blocks until the DMA buffer is consumed, providing natural pacing.
 *
 * psg: pointer to an initialised sn76489_t (must have sample_rate set to
 *      SN76489_AUDIO_SAMPLE_RATE at init time).
 * -------------------------------------------------------------------------- */
void sn76489_audio_push(sn76489_t *psg)
{
    if (!s_i2s_tx_chan) return;

    /* Render mono samples */
    int16_t mono[SN76489_AUDIO_DMA_BUF_LEN];
    sn76489_render(psg, mono, SN76489_AUDIO_DMA_BUF_LEN);

    /* Expand mono → stereo interleaved (L, R) */
    for (int i = 0; i < SN76489_AUDIO_DMA_BUF_LEN; i++) {
        s_dma_buf[i * 2]     = mono[i];
        s_dma_buf[i * 2 + 1] = mono[i];
    }

    size_t written = 0;
    esp_err_t err = i2s_channel_write(
        s_i2s_tx_chan,
        s_dma_buf,
        sizeof(s_dma_buf),
        &written,
        portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s_channel_write error: %s", esp_err_to_name(err));
    }
}

/* --------------------------------------------------------------------------
 * Public: stop and free I2S channel
 * -------------------------------------------------------------------------- */
void sn76489_audio_deinit(void)
{
    if (s_i2s_tx_chan) {
        i2s_channel_disable(s_i2s_tx_chan);
        i2s_del_channel(s_i2s_tx_chan);
        s_i2s_tx_chan = NULL;
        ESP_LOGI(TAG, "I2S deinit OK");
    }
}

/* --------------------------------------------------------------------------
 * Public: query configured sample rate
 * -------------------------------------------------------------------------- */
uint32_t sn76489_audio_sample_rate(void)
{
    return SN76489_AUDIO_SAMPLE_RATE;
}

#endif /* ESP_PLATFORM */
