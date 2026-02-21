/*
 * sn76489_audio.c — DAC audio bridge for SN76489 PSG on ESP32
 *
 * Uses the ESP32 internal DAC on GPIO 25 (DAC channel 1) via the
 * esp_driver_dac continuous-mode API (ESP-IDF v5.x).
 *
 * The BBC SN76489 produces 16-bit signed mono PCM.  We scale each sample
 * to the DAC's 8-bit unsigned range [0, 255] with a DC offset of 128.
 *
 * GPIO 25 is connected directly to the 3.5 mm audio jack on the Olimex
 * ESP32-SBC-FabGL board (same as TTGO VGA32).  No external BCK/WS needed.
 *
 * Usage:
 *   1. Call sn76489_audio_init() once after sn76489_init().
 *   2. Call sn76489_audio_push(&psg) from a FreeRTOS task (loops forever).
 *   3. Call sn76489_audio_deinit() on shutdown.
 *
 * Licence: GPL-2.0
 * Copyright (c) 2026 esp-beep project
 */

#ifdef ESP_PLATFORM

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "driver/dac_continuous.h"
#include "sn76489.h"

static const char *TAG = "sn76489_audio";

/* --------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------- */
#ifndef SN76489_AUDIO_SAMPLE_RATE
#  define SN76489_AUDIO_SAMPLE_RATE   22050
#endif

/*
 * DMA buffer length in samples.
 * dac_continuous_write_cyclically() copies data into its internal ring
 * buffer; we keep this small to minimise latency.
 */
#ifndef SN76489_AUDIO_BUF_SAMPLES
#  define SN76489_AUDIO_BUF_SAMPLES   512
#endif

/* --------------------------------------------------------------------------
 * Module state
 * -------------------------------------------------------------------------- */
static dac_continuous_handle_t s_dac_handle = NULL;

/* Intermediate 8-bit unsigned PCM buffer */
static uint8_t s_dac_buf[SN76489_AUDIO_BUF_SAMPLES];

/* --------------------------------------------------------------------------
 * Public: initialise DAC peripheral
 * -------------------------------------------------------------------------- */
esp_err_t sn76489_audio_init(void)
{
    dac_continuous_config_t cfg = {
        .chan_mask   = DAC_CHANNEL_MASK_CH0,   /* GPIO 25 = DAC1 = channel 0 */
        .desc_num    = 4,                       /* number of DMA descriptors  */
        .buf_size    = SN76489_AUDIO_BUF_SAMPLES * 4, /* internal ring buf    */
        .freq_hz     = SN76489_AUDIO_SAMPLE_RATE,
        .offset      = 0,
        .clk_src     = DAC_DIGI_CLK_SRC_DEFAULT,
        .chan_mode    = DAC_CHANNEL_MODE_SIMUL,
    };

    esp_err_t err = dac_continuous_new_channels(&cfg, &s_dac_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dac_continuous_new_channels failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = dac_continuous_enable(s_dac_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dac_continuous_enable failed: %s",
                 esp_err_to_name(err));
        dac_continuous_del_channels(s_dac_handle);
        s_dac_handle = NULL;
        return err;
    }

    ESP_LOGI(TAG, "DAC audio init OK: %d Hz on GPIO 25",
             SN76489_AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Public: render one buffer and push it to the DAC
 *
 * The SN76489 renders signed 16-bit mono; we convert to 8-bit unsigned
 * (add 32768, shift right 8) and write cyclically to the DAC DMA ring.
 * dac_continuous_write_cyclically() blocks until the buffer fits.
 * -------------------------------------------------------------------------- */
void sn76489_audio_push(sn76489_t *psg)
{
    if (!s_dac_handle) return;

    /* Render signed 16-bit mono samples */
    int16_t mono[SN76489_AUDIO_BUF_SAMPLES];
    sn76489_render(psg, mono, SN76489_AUDIO_BUF_SAMPLES);

    /* Convert signed 16-bit → unsigned 8-bit for DAC */
    for (int i = 0; i < SN76489_AUDIO_BUF_SAMPLES; i++) {
        /* mono[i] is in [-32768, 32767]; map to [0, 255] */
        int32_t v = (int32_t)mono[i] + 32768;   /* → [0, 65535] */
        s_dac_buf[i] = (uint8_t)(v >> 8);        /* → [0, 255]   */
    }

    size_t loaded = 0;
    esp_err_t err = dac_continuous_write_cyclically(
        s_dac_handle,
        s_dac_buf,
        sizeof(s_dac_buf),
        &loaded);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dac_continuous_write_cyclically error: %s",
                 esp_err_to_name(err));
    }
}

/* --------------------------------------------------------------------------
 * Public: stop and free DAC channel
 * -------------------------------------------------------------------------- */
void sn76489_audio_deinit(void)
{
    if (s_dac_handle) {
        dac_continuous_disable(s_dac_handle);
        dac_continuous_del_channels(s_dac_handle);
        s_dac_handle = NULL;
        ESP_LOGI(TAG, "DAC audio deinit OK");
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
