/*
 * bbc_tape.c — BBC Micro tape (UEF) emulation
 *
 * UEF format reference: http://electrem.emuunlim.com/UEFSpecs.htm
 *
 * We parse UEF at load time into a flat list of tape blocks.
 * Each UEF chunk 0x0100 = one BBC tape block (sync byte + header + data).
 *
 * The ACIA (MC6850-compatible Serial ULA) delivers bytes to the MOS.
 * Baud rate: 1200 baud = 1 byte per ~16667 cycles at 2 MHz.
 * We use a simple cycle counter to pace delivery.
 *
 * ACIA register map (BBC Micro):
 *   $FE08 write = control   $FE08 read = status
 *   $FE09 read  = RX data   $FE09 write = TX data (ignored)
 *
 * Status register bits:
 *   bit 0  RDRF  Receive Data Register Full
 *   bit 1  TDRE  Transmit Data Register Empty (always 1)
 *   bit 2  DCD   Data Carrier Detect (0=carrier/motor on, 1=no carrier)
 *   bit 3  CTS   Clear To Send (always 0)
 *   bit 7  IRQ   (RDRF & rx_irq_en) or (DCD changed)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bbc_tape.h"

#ifndef ESP_PLATFORM
#  include <zlib.h>   /* for gzip decompression on host */
#endif

#ifdef ESP_PLATFORM
#  include "esp_log.h"
#  define TAPE_LOGI(fmt, ...) ESP_LOGI("bbc_tape", fmt, ##__VA_ARGS__)
#  define TAPE_LOGD(fmt, ...) ESP_LOGD("bbc_tape", fmt, ##__VA_ARGS__)
#  define TAPE_LOGW(fmt, ...) ESP_LOGW("bbc_tape", fmt, ##__VA_ARGS__)
#else
#  include <stdio.h>
#  define TAPE_LOGI(fmt, ...) fprintf(stderr, "[tape] " fmt "\n", ##__VA_ARGS__)
#  define TAPE_LOGD(fmt, ...) ((void)0)
#  define TAPE_LOGW(fmt, ...) fprintf(stderr, "[tape] WARN: " fmt "\n", ##__VA_ARGS__)
#endif

/* Cycles per byte at 1200 baud, 2 MHz clock.
 * 1200 baud, 8N1 = 10 bits per byte → 2000000/1200/10 ≈ 167 cycles/bit
 * Actually BBC tape is 1200 baud with start+8data+1stop = 10 bits
 * so cycles_per_byte = 2000000 / 1200 ≈ 1667 */
#define CYCLES_PER_BYTE  1667

/* -------------------------------------------------------------------------
 * UEF helpers — little-endian reads from raw buffer
 * ------------------------------------------------------------------------- */
static uint16_t read_u16le(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t read_u32le(const uint8_t *p) {
    return (uint32_t)(p[0] | ((uint32_t)p[1]<<8) |
                      ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24));
}

/* -------------------------------------------------------------------------
 * bbc_tape_init
 * ------------------------------------------------------------------------- */
void bbc_tape_init(bbc_tape_t *tape)
{
    memset(tape, 0, sizeof(*tape));
}

/* -------------------------------------------------------------------------
 * bbc_tape_free
 * ------------------------------------------------------------------------- */
void bbc_tape_free(bbc_tape_t *tape)
{
    if (tape->uef_data) { free(tape->uef_data); tape->uef_data = NULL; }
    if (tape->blocks)   { free(tape->blocks);   tape->blocks   = NULL; }
    tape->n_blocks = 0;
}

/* -------------------------------------------------------------------------
 * bbc_tape_load_uef
 *
 * UEF file structure:
 *   10 bytes header: "UEF File!\0" + minor_ver + major_ver
 *   Then chunks:
 *     2 bytes chunk ID (LE)
 *     4 bytes chunk length (LE)
 *     N bytes chunk data
 *
 * Chunk 0x0100 = tape data block. The chunk data IS the raw tape block
 * bytes (sync byte 0x2A + BBC tape block header + data + CRC).
 * We store each 0x0100 chunk as one bbc_tape_block_t.
 * ------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------
 * uef_decompress — load file into memory, decompressing gzip if needed.
 * Returns malloc'd buffer (caller must free) and sets *out_size.
 * Returns NULL on error.
 * ------------------------------------------------------------------------- */
static uint8_t *uef_load_raw(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 12) { fclose(f); return NULL; }

    uint8_t *raw = (uint8_t *)malloc((size_t)sz);
    if (!raw) { fclose(f); return NULL; }
    if ((long)fread(raw, 1, (size_t)sz, f) != sz) {
        free(raw); fclose(f); return NULL;
    }
    fclose(f);

    /* Check for gzip magic: 1F 8B */
    if (raw[0] == 0x1F && raw[1] == 0x8B) {
#ifndef ESP_PLATFORM
        /* Decompress using zlib inflate with gzip wrapper */
        /* Estimate decompressed size: try 8× first, grow if needed */
        size_t out_cap = (size_t)sz * 8;
        if (out_cap < 65536) out_cap = 65536;
        uint8_t *out_buf = (uint8_t *)malloc(out_cap);
        if (!out_buf) { free(raw); return NULL; }

        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        zs.next_in  = raw;
        zs.avail_in = (uInt)sz;

        /* inflateInit2 with windowBits=47 = gzip + zlib auto-detect */
        if (inflateInit2(&zs, 47) != Z_OK) {
            free(out_buf); free(raw); return NULL;
        }

        zs.next_out  = out_buf;
        zs.avail_out = (uInt)out_cap;

        int ret = inflate(&zs, Z_FINISH);
        if (ret == Z_BUF_ERROR || ret == Z_OK) {
            /* Buffer too small — try larger */
            size_t filled = out_cap - zs.avail_out;
            out_cap *= 4;
            uint8_t *bigger = (uint8_t *)realloc(out_buf, out_cap);
            if (!bigger) { inflateEnd(&zs); free(out_buf); free(raw); return NULL; }
            out_buf = bigger;
            zs.next_out  = out_buf + filled;
            zs.avail_out = (uInt)(out_cap - filled);
            ret = inflate(&zs, Z_FINISH);
        }

        size_t decompressed = out_cap - zs.avail_out;
        inflateEnd(&zs);
        free(raw);

        if (ret != Z_STREAM_END) {
            TAPE_LOGW("gzip decompress failed (ret=%d)", ret);
            free(out_buf); return NULL;
        }

        TAPE_LOGI("gzip decompressed %ld -> %zu bytes", sz, decompressed);
        *out_size = decompressed;
        return out_buf;
#else
        TAPE_LOGW("gzip UEF not supported on ESP32 (no zlib)");
        free(raw); return NULL;
#endif
    }

    *out_size = (size_t)sz;
    return raw;
}

int bbc_tape_load_uef(bbc_tape_t *tape, const char *path)
{
    bbc_tape_free(tape);

    size_t uef_size = 0;
    uint8_t *uef_data = uef_load_raw(path, &uef_size);
    if (!uef_data) { TAPE_LOGW("cannot load %s", path); return -1; }

    /* Verify UEF magic */
    if (uef_size < 12 || memcmp(uef_data, "UEF File!", 9) != 0) {
        TAPE_LOGW("bad UEF magic in %s", path);
        free(uef_data); return -1;
    }

    tape->uef_data = uef_data;
    tape->uef_size = uef_size;

    /* Count valid 0x0100 chunks (len >= 2 = at least sync + 1 byte) */
    uint16_t n = 0;
    size_t pos = 12;
    while (pos + 6 <= tape->uef_size) {
        uint16_t chunk_id  = read_u16le(tape->uef_data + pos);
        uint32_t chunk_len = read_u32le(tape->uef_data + pos + 2);
        pos += 6;
        if (chunk_id == UEF_CHUNK_DATA && chunk_len >= 2) n++;
        if (pos + chunk_len > tape->uef_size) break;
        pos += chunk_len;
    }

    if (n == 0) {
        TAPE_LOGW("no data chunks in %s", path);
        bbc_tape_free(tape); return -1;
    }

    tape->blocks = (bbc_tape_block_t *)calloc(n, sizeof(bbc_tape_block_t));
    if (!tape->blocks) { bbc_tape_free(tape); return -1; }
    tape->n_blocks = n;

    /* Second pass: fill blocks */
    uint16_t bi = 0;
    pos = 12;
    while (pos + 6 <= tape->uef_size && bi < n) {
        uint16_t chunk_id  = read_u16le(tape->uef_data + pos);
        uint32_t chunk_len = read_u32le(tape->uef_data + pos + 2);
        pos += 6;
        if (chunk_id == UEF_CHUNK_DATA && chunk_len >= 2) {
            uint16_t blen = (chunk_len > BBC_TAPE_BLOCK_MAX)
                            ? BBC_TAPE_BLOCK_MAX
                            : (uint16_t)chunk_len;
            memcpy(tape->blocks[bi].data, tape->uef_data + pos, blen);
            tape->blocks[bi].len = blen;
            bi++;
        }
        if (pos + chunk_len > tape->uef_size) break;
        pos += chunk_len;
    }

    TAPE_LOGI("loaded %s: %d tape blocks", path, n);
    return 0;
}

/* -------------------------------------------------------------------------
 * bbc_tape_set_irq_cb
 * ------------------------------------------------------------------------- */
void bbc_tape_set_irq_cb(bbc_tape_t *tape,
                          void (*cb)(void *ctx, bool state), void *ctx)
{
    tape->irq_cb  = cb;
    tape->irq_ctx = ctx;
}

/* -------------------------------------------------------------------------
 * bbc_tape_set_motor
 * ------------------------------------------------------------------------- */
void bbc_tape_set_motor(bbc_tape_t *tape, bool on)
{
    if (tape->motor_on == on) return;
    tape->motor_on = on;
    TAPE_LOGI("motor %s", on ? "ON" : "OFF");
    if (on) {
        /* Give the MOS ~200ms to configure the ACIA before first byte arrives.
         * 200ms * 2MHz = 400000 cycles delay. */
        tape->cycle_acc = -400000;
        tape->rx_full   = false;
        tape->running   = true;
    }
}

/* -------------------------------------------------------------------------
 * ACIA status byte — matches MC6850 Verilog:
 *   status = {~irq_n, rx_pe, rx_ovr, rx_fe, cts_n, dcd_n, ~tdr_full, rx_full}
 *   bit 0 = RDRF  (rx_full)
 *   bit 1 = TDRE  (~tdr_full, always 1 for us — we don't transmit)
 *   bit 2 = DCD   (0 = carrier present = motor on; 1 = no carrier)
 *   bit 3 = CTS   (0 = ok)
 *   bit 4 = FE    framing error (always 0)
 *   bit 5 = OVRN  overrun (always 0)
 *   bit 6 = PE    parity error (always 0)
 *   bit 7 = IRQ   (~irq_n) = rx_ie & (rx_full | ovr)
 * ------------------------------------------------------------------------- */
static uint8_t acia_status(const bbc_tape_t *tape)
{
    uint8_t s = 0x02;                  /* TDRE always 1 */
    if (tape->rx_full)   s |= 0x01;   /* RDRF */
    if (!tape->motor_on) s |= 0x04;   /* DCD: 1=no carrier */
    /* IRQ = rx_ie & (rx_full | ovr).  We have no ovr, so just rx_full. */
    if (tape->irq_enabled && tape->rx_full) s |= 0x80;
    return s;
}

/* -------------------------------------------------------------------------
 * bbc_tape_read
 *   reg=0 → status register ($FE08)
 *   reg=1 → receive data register ($FE09)
 * ------------------------------------------------------------------------- */
uint8_t bbc_tape_read(bbc_tape_t *tape, uint8_t reg)
{
    if (reg == 0) {
        uint8_t s = acia_status(tape);
        TAPE_LOGD("ACIA status=%02X", s);
        return s;
    } else {
        /* Reading RDR clears RDRF and deasserts IRQ */
        uint8_t d = tape->rx_data;
        tape->rx_full = false;
        /* Update IRQ line */
        if (tape->irq_cb) tape->irq_cb(tape->irq_ctx, false);
        TAPE_LOGI("RX %02X", d);
        return d;
    }
}

/* -------------------------------------------------------------------------
 * bbc_tape_write
 *   reg=0 → control register ($FE08)
 *   reg=1 → TX data ($FE09, ignored for tape load)
 *
 * MC6850 control register (from Verilog):
 *   bits 1:0 = clk_mult / master reset (11 = master reset)
 *   bit  2   = parity_odd
 *   bits 4:3 = word select
 *   bits 6:5 = TX control
 *   bit  7   = rx_ie (RX interrupt enable)
 * ------------------------------------------------------------------------- */
void bbc_tape_write(bbc_tape_t *tape, uint8_t reg, uint8_t val)
{
    if (reg == 0) {
        tape->acia_control = val;
        bool old_irq_en = tape->irq_enabled;
        tape->irq_enabled = (val & 0x80) ? true : false;

        /* Master reset: bits 1:0 = 11 */
        if ((val & 0x03) == 0x03) {
            tape->rx_full = false;
            if (tape->irq_cb) tape->irq_cb(tape->irq_ctx, false);
            TAPE_LOGI("ACIA master reset");
        } else if (!old_irq_en && tape->irq_enabled && tape->rx_full) {
            /* IRQ just enabled and byte already waiting — assert IRQ now */
            if (tape->irq_cb) tape->irq_cb(tape->irq_ctx, true);
        }
        TAPE_LOGI("ACIA ctrl=%02X rx_ie=%d", val, tape->irq_enabled);
    }
    /* TX data writes ignored (we only emulate RX) */
}

/* -------------------------------------------------------------------------
 * bbc_tape_tick
 *
 * Called every bbc_machine_step with the number of CPU cycles elapsed.
 * When the motor is on and a block is available, delivers bytes at
 * 1200 baud pace.
 * ------------------------------------------------------------------------- */
void bbc_tape_tick(bbc_tape_t *tape, int cycles)
{
    /* Only deliver once the motor has been turned on at least once.
     * After that, keep streaming even if motor is turned off (relay inertia).
     * The DCD bit in status reflects motor_on; the MOS uses DCD for error detection. */
    if (!tape->running) return;
    if (!tape->blocks || tape->cur_block >= tape->n_blocks) return;
    if (tape->rx_full) return;  /* MOS hasn't read the previous byte yet */

    tape->cycle_acc += cycles;
    if (tape->cycle_acc < CYCLES_PER_BYTE) return;
    tape->cycle_acc -= CYCLES_PER_BYTE;

    /* Deliver next byte */
    bbc_tape_block_t *blk = &tape->blocks[tape->cur_block];
    tape->rx_data = blk->data[tape->cur_pos++];
    tape->rx_full = true;

    if (tape->cur_pos == 1)
        TAPE_LOGI("delivering block %d (%d bytes)", tape->cur_block, blk->len);
    TAPE_LOGD("deliver block=%d pos=%d byte=%02X",
              tape->cur_block, tape->cur_pos - 1, tape->rx_data);

    /* Advance to next block if done */
    if (tape->cur_pos >= blk->len) {
        tape->cur_pos = 0;
        tape->cur_block++;
        if (tape->cur_block >= tape->n_blocks)
            TAPE_LOGI("tape end");
    }

    /* Assert IRQ if enabled */
    if (tape->irq_enabled && tape->irq_cb)
        tape->irq_cb(tape->irq_ctx, true);
}
