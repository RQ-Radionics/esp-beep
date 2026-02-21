/*
 * sn76489.h — SN76489 PSG emulation for BBC Micro / ESP32-ESP-IDF
 *
 * BBC Model B variant:
 *   - Clock input: 4 MHz (divided by 16 internally → 250 kHz base)
 *   - 3 tone channels + 1 noise channel
 *   - LFSR: 15-bit, seed 0x4000, white-noise feedback = bit0 XOR bit1,
 *     periodic-noise feedback = bit0 only
 *   - Volume: 16 levels, 2 dB/step, level 0 = max, level 15 = silence
 *   - No malloc; all state in sn76489_t
 *
 * Derived from emu76489 by Mitsutaka Okazaki (MIT) and b-em sound.c by
 * Tom Walker (GPL-2).  This adaptation is GPL-2.0.
 *
 * Copyright (c) 2026 esp-beep project
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

/* Number of audio channels (3 tone + 1 noise) */
#define SN76489_CHANNELS    4

/* Channel indices */
#define SN76489_TONE0       0
#define SN76489_TONE1       1
#define SN76489_TONE2       2
#define SN76489_NOISE       3

/* Chip clock in Hz (BBC Model B) */
#define SN76489_CLOCK_HZ    4000000UL

/* Internal clock divisor */
#define SN76489_CLOCK_DIV   16

/* Base rate = CLOCK / DIV */
#define SN76489_BASE_RATE   (SN76489_CLOCK_HZ / SN76489_CLOCK_DIV)

/* LFSR constants — BBC 15-bit variant */
#define SN76489_LFSR_SEED   0x4000u
#define SN76489_LFSR_MASK   0x7FFFu

/* Noise-mode register bits (written to noise channel) */
#define SN76489_NOISE_WHITE     0x04   /* bit 2: 1 = white, 0 = periodic */
#define SN76489_NOISE_RATE_MASK 0x03   /* bits 1:0 = rate select         */

/* --------------------------------------------------------------------------
 * Per-channel state
 * -------------------------------------------------------------------------- */
typedef struct {
    uint16_t tone;      /* tone/noise divider value (11-bit for tone)  */
    uint16_t counter;   /* current down-counter                        */
    uint8_t  volume;    /* attenuation: 0 = max, 15 = silence          */
    int8_t   output;    /* current square-wave output: +1 or -1        */
} sn76489_channel_t;

/* --------------------------------------------------------------------------
 * Full chip state — no heap allocation needed
 * -------------------------------------------------------------------------- */
typedef struct {
    sn76489_channel_t ch[SN76489_CHANNELS];

    /* Noise-channel LFSR state */
    uint16_t lfsr;
    bool     noise_white;     /* true = white noise, false = periodic   */
    uint8_t  noise_rate;      /* 0-2: fixed rates; 3: tracks tone2      */

    /* Latch tracking: which channel/register is being updated          */
    uint8_t  latch_ch;        /* latched channel index (0-3)            */
    bool     latch_is_vol;    /* true = volume register latched         */

    /* Sample-rate conversion accumulator                               */
    uint32_t sample_rate;     /* output sample rate in Hz               */
    uint32_t acc;             /* fractional accumulator (base-rate units)*/

    /* Logarithmic volume table: vol_table[0..15] → linear amplitude   */
    /* Computed once at init from 2 dB/step. Stored as 0-0x7FFF.       */
    uint16_t vol_table[16];
} sn76489_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/*
 * Initialise chip state.
 *
 * sample_rate: output PCM sample rate in Hz (e.g. 44100, 22050).
 *              Pass 0 to skip rate-conversion setup (tick-only mode).
 */
void sn76489_init(sn76489_t *psg, uint32_t sample_rate);

/* Hardware reset — silences all channels, resets LFSR seed */
void sn76489_reset(sn76489_t *psg);

/*
 * Write a byte to the chip.
 * This is the only write path; the chip uses an internal latch to
 * distinguish tone/volume writes from second-byte data writes.
 *
 * On the BBC Model B: called from bbc_sysvia when IC32 bit 0 goes LOW
 * (sound WE asserted) with Port A as data.
 */
void sn76489_write(sn76489_t *psg, uint8_t data);

/*
 * Tick the chip by `clocks` base-rate clocks (i.e. 250 kHz ticks for BBC).
 *
 * Use this when driving the chip from a CPU cycle counter.
 * Each 1 MHz CPU cycle = 4 base-rate clocks (4 MHz / 16 * 16 / 1).
 *
 * Note: sn76489_render() already handles clocking internally; only use
 * sn76489_tick() if you do NOT use sn76489_render().
 */
void sn76489_tick(sn76489_t *psg, uint32_t clocks);

/*
 * Render `n_samples` of 16-bit signed mono PCM into `buf`.
 *
 * The function advances the chip clock by the number of base-rate cycles
 * that correspond to n_samples at the configured sample_rate, using a
 * fractional accumulator for accuracy.
 *
 * Output range: -32768 .. +32767 (sum of all four channels).
 */
void sn76489_render(sn76489_t *psg, int16_t *buf, uint32_t n_samples);

/*
 * Read back current volume attenuation of a channel (0=max, 15=silence).
 */
uint8_t sn76489_get_volume(const sn76489_t *psg, uint8_t channel);

/*
 * Read back current tone divider of a channel (0-2: tone, 3: noise rate).
 */
uint16_t sn76489_get_tone(const sn76489_t *psg, uint8_t channel);

/* --------------------------------------------------------------------------
 * DAC audio bridge (ESP32 only — sn76489_audio.c)
 * Uses the ESP32 internal DAC on GPIO 25 (DAC channel 1, 3.5mm jack).
 * -------------------------------------------------------------------------- */
#ifdef ESP_PLATFORM
#include "esp_err.h"

/*
 * Initialise the internal DAC for audio output.
 * Must be called after sn76489_init().
 * Sample rate is a compile-time constant (see sn76489_audio.c);
 * override with -DSTN76489_AUDIO_SAMPLE_RATE=<hz> if needed.
 */
esp_err_t sn76489_audio_init(void);

/*
 * Render one DMA buffer and push it to I2S.
 * Call this in a loop from a dedicated FreeRTOS task.
 * The function blocks until the DMA buffer is consumed.
 */
void sn76489_audio_push(sn76489_t *psg);

/* Stop and free the I2S channel. */
void sn76489_audio_deinit(void);

/* Return the configured sample rate in Hz. */
uint32_t sn76489_audio_sample_rate(void);

#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif
