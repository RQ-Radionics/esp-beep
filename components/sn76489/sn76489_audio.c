/*
 * sn76489_audio.c — DAC audio bridge for SN76489 PSG on ESP32
 *
 * Uses the ESP32 internal DAC on GPIO 25 (DAC channel 1) via the
 * esp_driver_dac continuous-mode API (ESP-IDF v5.x) in ASYNC mode.
 *
 * The BBC SN76489 produces 16-bit signed mono PCM.  We scale each sample
 * to the DAC's 8-bit unsigned range [0, 255] with a DC offset of 128.
 *
 * GPIO 25 is connected directly to the 3.5 mm audio jack on the Olimex
 * ESP32-SBC-FabGL board (same as TTGO VGA32).  No external BCK/WS needed.
 *
 * Async design
 * ------------
 * dac_continuous_write_cyclically() contains a bare spin-wait
 * (`while (atomic_load(&handle->is_running)) {}`) that holds the CPU for
 * the entire duration of a DMA transfer (~23 ms at 22 kHz / 512 samples).
 * This starves the FreeRTOS IDLE task and triggers the Task Watchdog.
 *
 * Instead we use the async API:
 *   - Register on_convert_done callback (called from DMA ISR).
 *   - The ISR posts the free DMA buffer pointer to a FreeRTOS queue.
 *   - sn76489_audio_push() blocks on that queue (yields CPU while waiting)
 *     and then fills the buffer with new PCM data.
 *
 * Usage:
 *   1. Call sn76489_audio_init() once after sn76489_init().
 *   2. Call sn76489_audio_push(&psg) from a FreeRTOS task in a loop.
 *   3. Call sn76489_audio_deinit() on shutdown.
 *
 * Licence: GPL-2.0
 * Copyright (c) 2026 esp-beep project
 */

#ifdef ESP_PLATFORM

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
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
 * Number of DMA descriptors / buffers.
 * Each buffer holds SN76489_AUDIO_BUF_SAMPLES bytes (8-bit DAC data).
 * More descriptors = more audio latency but smoother scheduling.
 */
#ifndef SN76489_AUDIO_DESC_NUM
/* 2 descriptors: one being consumed by DMA, one being filled by the task.
 * More descriptors increases latency and lets the queue accumulate events,
 * which prevents audioTask from blocking and starves IDLE1. */
#  define SN76489_AUDIO_DESC_NUM      2
#endif

/*
 * DMA buffer size in bytes (== samples, since DAC is 8-bit).
 * Must be in the range [32, 4092] per the driver docs.
 * 256 samples @ 22050 Hz ≈ 11.6 ms per buffer — short enough to keep
 * latency low, large enough to avoid starving the DMA.
 */
#ifndef SN76489_AUDIO_BUF_SAMPLES
#  define SN76489_AUDIO_BUF_SAMPLES   256
#endif

/* --------------------------------------------------------------------------
 * Module state
 * -------------------------------------------------------------------------- */
static dac_continuous_handle_t s_dac_handle = NULL;

/*
 * The ISR posts dac_event_data_t* into this queue whenever a DMA buffer
 * finishes converting and is ready to be refilled.
 */
static QueueHandle_t s_dma_queue = NULL;

/* --------------------------------------------------------------------------
 * ISR callback — called from DMA interrupt context
 *
 * Posts the event (which carries the free DMA buffer pointer) to the queue
 * so the audio task can refill it.  Returns true if a higher-priority task
 * was woken (standard ISR convention for FreeRTOS).
 * -------------------------------------------------------------------------- */
static IRAM_ATTR bool on_convert_done(dac_continuous_handle_t handle,
                                      const dac_event_data_t *event,
                                      void *user_data)
{
    BaseType_t high_prio_woken = pdFALSE;
    xQueueSendFromISR(s_dma_queue, event, &high_prio_woken);
    return high_prio_woken == pdTRUE;
}

/* --------------------------------------------------------------------------
 * Public: initialise DAC peripheral in async mode
 * -------------------------------------------------------------------------- */
esp_err_t sn76489_audio_init(void)
{
    /* Queue depth == descriptor count so the ISR never blocks */
    s_dma_queue = xQueueCreate(SN76489_AUDIO_DESC_NUM, sizeof(dac_event_data_t));
    if (!s_dma_queue) {
        ESP_LOGE(TAG, "Failed to create DMA event queue");
        return ESP_ERR_NO_MEM;
    }

    dac_continuous_config_t cfg = {
        .chan_mask   = DAC_CHANNEL_MASK_CH0,        /* GPIO 25 = DAC1 = channel 0 */
        .desc_num    = SN76489_AUDIO_DESC_NUM,
        .buf_size    = SN76489_AUDIO_BUF_SAMPLES,   /* bytes per DMA descriptor   */
        .freq_hz     = SN76489_AUDIO_SAMPLE_RATE,
        .offset      = 0,
        .clk_src     = DAC_DIGI_CLK_SRC_DEFAULT,
        .chan_mode    = DAC_CHANNEL_MODE_SIMUL,
    };

    esp_err_t err = dac_continuous_new_channels(&cfg, &s_dac_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dac_continuous_new_channels failed: %s",
                 esp_err_to_name(err));
        vQueueDelete(s_dma_queue);
        s_dma_queue = NULL;
        return err;
    }

    /* Register the ISR callback before enabling */
    dac_event_callbacks_t cbs = {
        .on_convert_done = on_convert_done,
        .on_stop         = NULL,
    };
    err = dac_continuous_register_event_callback(s_dac_handle, &cbs, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dac_continuous_register_event_callback failed: %s",
                 esp_err_to_name(err));
        dac_continuous_del_channels(s_dac_handle);
        s_dac_handle = NULL;
        vQueueDelete(s_dma_queue);
        s_dma_queue = NULL;
        return err;
    }

    err = dac_continuous_enable(s_dac_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dac_continuous_enable failed: %s",
                 esp_err_to_name(err));
        dac_continuous_del_channels(s_dac_handle);
        s_dac_handle = NULL;
        vQueueDelete(s_dma_queue);
        s_dma_queue = NULL;
        return err;
    }

    err = dac_continuous_start_async_writing(s_dac_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dac_continuous_start_async_writing failed: %s",
                 esp_err_to_name(err));
        dac_continuous_disable(s_dac_handle);
        dac_continuous_del_channels(s_dac_handle);
        s_dac_handle = NULL;
        vQueueDelete(s_dma_queue);
        s_dma_queue = NULL;
        return err;
    }

    ESP_LOGI(TAG, "DAC audio init OK (async): %d Hz on GPIO 25, "
             "%d descriptors x %d bytes",
             SN76489_AUDIO_SAMPLE_RATE,
             SN76489_AUDIO_DESC_NUM,
             SN76489_AUDIO_BUF_SAMPLES);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Public: render one buffer and push it to the DAC (async, non-blocking CPU)
 *
 * Blocks on the DMA event queue until a buffer is free (FreeRTOS sleep —
 * IDLE task runs normally), then fills it with freshly rendered PCM.
 * -------------------------------------------------------------------------- */
void sn76489_audio_push(sn76489_t *psg)
{
    if (!s_dac_handle || !s_dma_queue) return;

    /* Wait for a free DMA buffer.
     * Use a finite timeout (one full buffer period = ~12 ms at 22050/256 Hz)
     * instead of portMAX_DELAY so that if the queue is always full the task
     * still blocks long enough for IDLE1 to run and reset the TWDT. */
    dac_event_data_t event;
    if (xQueueReceive(s_dma_queue, &event, pdMS_TO_TICKS(20)) != pdTRUE) {
        /* Timeout: DAC stalled or queue empty — yield and retry next call */
        taskYIELD();
        return;
    }

    /* psg lives in PSRAM (machine struct is PSRAM-allocated).  Accessing
     * PSRAM in a tight render loop causes frequent cache misses over the
     * slow SPI bus, which can take hundreds of milliseconds and starve
     * IDLE1.  Copy the chip state to IRAM-backed stack, render locally,
     * then write the mutated state back. */
    sn76489_t local_psg;
    memcpy(&local_psg, psg, sizeof(sn76489_t));

    /* Clamp acc: if corrupted (race with emulator memcpy or uninitialized
     * PSRAM) it can cause the render while-loop to spin millions of times.
     * A valid acc is always < sample_rate after any completed render call.
     * Also guard against a bogus sample_rate that slipped through. */
    if (local_psg.sample_rate != SN76489_AUDIO_SAMPLE_RATE) {
        ESP_LOGW(TAG, "sample_rate invalid: %lu — reinitialising PSG state",
                 (unsigned long)local_psg.sample_rate);
        local_psg.sample_rate = SN76489_AUDIO_SAMPLE_RATE;
        local_psg.acc = 0;
    } else if (local_psg.acc >= local_psg.sample_rate) {
        ESP_LOGW(TAG, "acc out of range: %lu — clamping",
                 (unsigned long)local_psg.acc);
        local_psg.acc = 0;
    }

    /* Render signed 16-bit mono samples into a stack buffer */
    int16_t mono[SN76489_AUDIO_BUF_SAMPLES];
    sn76489_render(&local_psg, mono, SN76489_AUDIO_BUF_SAMPLES);

    /* Write back only the mutable state (counters, LFSR, acc) */
    memcpy(psg, &local_psg, sizeof(sn76489_t));

    /* Convert signed 16-bit → unsigned 8-bit and write into the DMA buffer */
    size_t n = event.buf_size < SN76489_AUDIO_BUF_SAMPLES
               ? event.buf_size : SN76489_AUDIO_BUF_SAMPLES;
    uint8_t *dma_buf = (uint8_t *)event.buf;
    for (size_t i = 0; i < n; i++) {
        int32_t v = (int32_t)mono[i] + 32768;  /* [-32768,32767] → [0,65535] */
        dma_buf[i] = (uint8_t)(v >> 8);         /* → [0,255]                  */
    }

    size_t loaded = 0;
    esp_err_t err = dac_continuous_write_asynchronously(
        s_dac_handle,
        dma_buf,
        event.buf_size,
        dma_buf,
        n,
        &loaded);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dac_continuous_write_asynchronously error: %s",
                 esp_err_to_name(err));
    }
}

/* --------------------------------------------------------------------------
 * Public: stop and free DAC channel
 * -------------------------------------------------------------------------- */
void sn76489_audio_deinit(void)
{
    if (s_dac_handle) {
        dac_continuous_stop_async_writing(s_dac_handle);
        dac_continuous_disable(s_dac_handle);
        dac_continuous_del_channels(s_dac_handle);
        s_dac_handle = NULL;
        ESP_LOGI(TAG, "DAC audio deinit OK");
    }
    if (s_dma_queue) {
        vQueueDelete(s_dma_queue);
        s_dma_queue = NULL;
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
