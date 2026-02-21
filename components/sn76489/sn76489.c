/*
 * sn76489.c — SN76489 PSG emulation for BBC Micro / ESP32-ESP-IDF
 *
 * BBC Model B variant:
 *   Clock: 4 MHz input, ÷16 internally → 250 kHz base rate
 *   3 tone channels + 1 noise channel
 *   LFSR: 15-bit, seed 0x4000
 *     White noise  feedback: bit0 XOR bit1
 *     Periodic     feedback: bit0
 *   Volume: 16 levels, 2 dB/step, 0=max, 15=silence
 *
 * Derived from emu76489 by Mitsutaka Okazaki (MIT) and b-em sound.c
 * by Tom Walker (GPL-2).  This file is GPL-2.0.
 *
 * Copyright (c) 2026 esp-beep project
 */

#include <string.h>
#include <math.h>
#include "sn76489.h"

#ifdef ESP_PLATFORM
#  include "esp_log.h"
#  define SN_LOGD(fmt, ...) ESP_LOGD("sn76489", fmt, ##__VA_ARGS__)
#  define SN_LOGW(fmt, ...) ESP_LOGW("sn76489", fmt, ##__VA_ARGS__)
#else
#  include <stdio.h>
#  define SN_LOGD(fmt, ...) /* no-op */
#  define SN_LOGW(fmt, ...) fprintf(stderr, "sn76489 WARN: " fmt "\n", ##__VA_ARGS__)
#endif

/* --------------------------------------------------------------------------
 * Volume table
 *
 * The SN76489 uses a 4-bit attenuation register.  Each step = 2 dB.
 * Level 0 = full volume, level 15 = silence.
 * Amplitude = 32767 * 10^(-atten_dB / 20)  where atten_dB = level * 2.
 * -------------------------------------------------------------------------- */
static void build_vol_table(sn76489_t *psg)
{
    for (int i = 0; i < 15; i++) {
        double dB = (double)i * 2.0;
        double lin = 32767.0 * pow(10.0, -dB / 20.0);
        psg->vol_table[i] = (uint16_t)(lin + 0.5);
    }
    psg->vol_table[15] = 0;  /* silence */
}

/* --------------------------------------------------------------------------
 * Noise divider rates (BBC variant)
 *
 * Noise rate select bits 1:0:
 *   0 → divide tone-2 counter by 15  (BBC: not 16 as in Sega)
 *   1 → fixed N/16  (2 × tone)
 *   2 → fixed N/32  (4 × tone)
 *   3 → use tone channel 2 frequency
 *
 * The BBC variant uses ÷15 for rate 0, unlike the Sega ÷16.
 * -------------------------------------------------------------------------- */
static const uint16_t s_noise_dividers[3] = { 15, 32, 64 };

/*
 * Minimum tone period that produces an audible frequency (< 20 kHz).
 *
 * With BBC clock 4 MHz / 16 = 250 kHz base rate:
 *   f = 250000 / (2 * period)
 *   period 1 → 125 kHz
 *   period 5 → 25 kHz   (above Nyquist for 44.1/48 kHz systems)
 *   period 6 → 20.8 kHz (first audible count at 44.1 kHz Nyquist)
 *
 * Matches VHDL generic MIN_PERIOD_CNT_G default of 6.
 * Periods 1..5 are "flat-lined": the toggle FF is held high so the channel
 * level can still be modulated (AM technique) without ultrasonic aliasing.
 */
#define SN76489_MIN_PERIOD  6

/* --------------------------------------------------------------------------
 * Internal: clock one channel by one base-rate tick.
 * -------------------------------------------------------------------------- */
static inline void tick_tone(sn76489_channel_t *ch)
{
    /* period=0: maximum period (counter wraps 0→0xFFFF); let it run. */
    if (ch->tone > 0 && ch->tone < SN76489_MIN_PERIOD) {
        /* Flat-line: hold output high, AM modulation still works via volume */
        ch->output = 1;
        return;
    }
    if (ch->counter > 0)
        ch->counter--;
    if (ch->counter == 0) {
        ch->counter = ch->tone;
        ch->output  = -ch->output;
    }
}

/* --------------------------------------------------------------------------
 * Internal: clock the noise channel by one base-rate tick.
 * -------------------------------------------------------------------------- */
static inline void tick_noise(sn76489_t *psg)
{
    sn76489_channel_t *nc = &psg->ch[SN76489_NOISE];

    /* Determine the effective divider */
    uint16_t divider;
    if (psg->noise_rate == 3) {
        /* Track tone channel 2 */
        divider = psg->ch[SN76489_TONE2].tone;
        if (divider == 0) divider = 1;
    } else {
        divider = s_noise_dividers[psg->noise_rate];
    }

    if (nc->counter > 0)
        nc->counter--;
    if (nc->counter == 0) {
        nc->counter = divider;

        /* Clock the LFSR */
        uint16_t feedback;
        if (psg->noise_white) {
            /* White noise: XOR of bit0 and bit1 (BBC variant) */
            feedback = (psg->lfsr ^ (psg->lfsr >> 1)) & 1u;
        } else {
            /* Periodic: bit0 only */
            feedback = psg->lfsr & 1u;
        }
        psg->lfsr = ((psg->lfsr >> 1) | (feedback << 14)) & SN76489_LFSR_MASK;
        nc->output = (psg->lfsr & 1u) ? 1 : -1;
    }
}

/* --------------------------------------------------------------------------
 * Internal: mix all channels into one 16-bit sample.
 * -------------------------------------------------------------------------- */
static inline int16_t mix_sample(const sn76489_t *psg)
{
    int32_t sum = 0;
    for (int i = 0; i < SN76489_CHANNELS; i++) {
        if (psg->ch[i].volume < 15) {
            int32_t amp = (int32_t)psg->vol_table[psg->ch[i].volume];
            sum += (int32_t)psg->ch[i].output * amp;
        }
    }
    /* Divide by number of channels to avoid clipping */
    sum /= SN76489_CHANNELS;
    if (sum >  32767) sum =  32767;
    if (sum < -32768) sum = -32768;
    return (int16_t)sum;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void sn76489_init(sn76489_t *psg, uint32_t sample_rate)
{
    memset(psg, 0, sizeof(*psg));
    psg->sample_rate = sample_rate;
    build_vol_table(psg);
    sn76489_reset(psg);
}

void sn76489_reset(sn76489_t *psg)
{
    for (int i = 0; i < SN76489_CHANNELS; i++) {
        psg->ch[i].tone    = 0;
        psg->ch[i].counter = 0;
        psg->ch[i].volume  = 15;   /* silence */
        psg->ch[i].output  = 1;
    }
    psg->lfsr        = SN76489_LFSR_SEED;
    psg->noise_white = false;
    psg->noise_rate  = 0;
    psg->latch_ch    = 0;
    psg->latch_is_vol = false;
    psg->acc         = 0;
    SN_LOGD("reset");
}

void sn76489_write(sn76489_t *psg, uint8_t data)
{
    if (data & 0x80) {
        /* LATCH byte: bit 6-5 = channel, bit 4 = type (0=tone/noise, 1=vol) */
        psg->latch_ch    = (data >> 5) & 0x03;
        psg->latch_is_vol = (data >> 4) & 0x01;

        if (psg->latch_is_vol) {
            /* Volume: bits 3:0 = attenuation */
            psg->ch[psg->latch_ch].volume = data & 0x0F;
            SN_LOGD("ch%d vol=%d", psg->latch_ch, psg->ch[psg->latch_ch].volume);
        } else {
            if (psg->latch_ch == SN76489_NOISE) {
                /* Noise control: bit 2 = white/periodic, bits 1:0 = rate */
                psg->noise_white = (data & SN76489_NOISE_WHITE) != 0;
                psg->noise_rate  = data & SN76489_NOISE_RATE_MASK;
                /* Reset LFSR on any noise write */
                psg->lfsr = SN76489_LFSR_SEED;
                psg->ch[SN76489_NOISE].counter = 0;
                SN_LOGD("noise white=%d rate=%d", psg->noise_white, psg->noise_rate);
            } else {
                /* Tone low 4 bits */
                psg->ch[psg->latch_ch].tone =
                    (psg->ch[psg->latch_ch].tone & 0x3F0) | (data & 0x0F);
                SN_LOGD("ch%d tone low=%d", psg->latch_ch,
                        psg->ch[psg->latch_ch].tone);
            }
        }
    } else {
        /* DATA byte: bits 5:0 = upper 6 bits of tone/vol */
        if (psg->latch_is_vol) {
            /* Volume data byte — same format as latch (lower 4 bits) */
            psg->ch[psg->latch_ch].volume = data & 0x0F;
            SN_LOGD("ch%d vol(data)=%d", psg->latch_ch,
                    psg->ch[psg->latch_ch].volume);
        } else {
            if (psg->latch_ch == SN76489_NOISE) {
                /* Noise: data byte repeats control */
                psg->noise_white = (data & SN76489_NOISE_WHITE) != 0;
                psg->noise_rate  = data & SN76489_NOISE_RATE_MASK;
                psg->lfsr = SN76489_LFSR_SEED;
                psg->ch[SN76489_NOISE].counter = 0;
            } else {
                /* Tone high 6 bits */
                psg->ch[psg->latch_ch].tone =
                    ((data & 0x3F) << 4) | (psg->ch[psg->latch_ch].tone & 0x0F);
                SN_LOGD("ch%d tone=%d", psg->latch_ch,
                        psg->ch[psg->latch_ch].tone);
            }
        }
    }
}

void sn76489_tick(sn76489_t *psg, uint32_t clocks)
{
    for (uint32_t i = 0; i < clocks; i++) {
        tick_tone(&psg->ch[SN76489_TONE0]);
        tick_tone(&psg->ch[SN76489_TONE1]);
        tick_tone(&psg->ch[SN76489_TONE2]);
        tick_noise(psg);
    }
}

void sn76489_render(sn76489_t *psg, int16_t *buf, uint32_t n_samples)
{
    /*
     * Rate conversion using a fractional accumulator.
     *
     * For each output sample we need to advance the chip by:
     *   base_rate / sample_rate   base-rate clocks
     *
     * We keep the fractional part in psg->acc (units: base-rate ticks × 1).
     * Multiply base_rate × n_samples and divide by sample_rate — but
     * do it per-sample to distribute the clock ticks evenly.
     */
    if (psg->sample_rate == 0) {
        /* No rate configured; output silence */
        memset(buf, 0, n_samples * sizeof(int16_t));
        return;
    }

    for (uint32_t s = 0; s < n_samples; s++) {
        psg->acc += SN76489_BASE_RATE;

        while (psg->acc >= psg->sample_rate) {
            psg->acc -= psg->sample_rate;
            tick_tone(&psg->ch[SN76489_TONE0]);
            tick_tone(&psg->ch[SN76489_TONE1]);
            tick_tone(&psg->ch[SN76489_TONE2]);
            tick_noise(psg);
        }

        buf[s] = mix_sample(psg);
    }
}

uint8_t sn76489_get_volume(const sn76489_t *psg, uint8_t channel)
{
    if (channel >= SN76489_CHANNELS) return 15;
    return psg->ch[channel].volume;
}

uint16_t sn76489_get_tone(const sn76489_t *psg, uint8_t channel)
{
    if (channel >= SN76489_CHANNELS) return 0;
    return psg->ch[channel].tone;
}
