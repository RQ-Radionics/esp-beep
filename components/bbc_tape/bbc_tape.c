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
int bbc_tape_load_uef(bbc_tape_t *tape, const char *path)
{
    bbc_tape_free(tape);

    FILE *f = fopen(path, "rb");
    if (!f) { TAPE_LOGW("cannot open %s", path); return -1; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 12) { fclose(f); return -1; }

    tape->uef_data = (uint8_t *)malloc((size_t)sz);
    if (!tape->uef_data) { fclose(f); return -1; }
    if ((long)fread(tape->uef_data, 1, (size_t)sz, f) != sz) {
        fclose(f); bbc_tape_free(tape); return -1;
    }
    fclose(f);
    tape->uef_size = (size_t)sz;

    /* Verify magic */
    if (memcmp(tape->uef_data, "UEF File!", 9) != 0) {
        TAPE_LOGW("bad UEF magic in %s", path);
        bbc_tape_free(tape);
        return -1;
    }

    /* Count 0x0100 chunks first */
    uint16_t n = 0;
    size_t pos = 12;  /* skip 10-byte magic + 2-byte version */
    while (pos + 6 <= tape->uef_size) {
        uint16_t chunk_id  = read_u16le(tape->uef_data + pos);
        uint32_t chunk_len = read_u32le(tape->uef_data + pos + 2);
        pos += 6;
        if (chunk_id == UEF_CHUNK_DATA) n++;
        pos += chunk_len;
    }

    if (n == 0) {
        TAPE_LOGW("no data chunks in %s", path);
        bbc_tape_free(tape);
        return -1;
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
        if (chunk_id == UEF_CHUNK_DATA && chunk_len > 0) {
            uint16_t blen = (chunk_len > BBC_TAPE_BLOCK_MAX)
                            ? BBC_TAPE_BLOCK_MAX
                            : (uint16_t)chunk_len;
            memcpy(tape->blocks[bi].data, tape->uef_data + pos, blen);
            tape->blocks[bi].len = blen;
            bi++;
        }
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
    TAPE_LOGD("motor %s", on ? "ON" : "OFF");
    /* Reset byte timer when motor starts */
    if (on) tape->rx_full = false;
}

/* -------------------------------------------------------------------------
 * ACIA status byte
 * ------------------------------------------------------------------------- */
static uint8_t acia_status(const bbc_tape_t *tape)
{
    uint8_t s = 0;
    if (tape->rx_full)   s |= 0x01;  /* RDRF */
    s |= 0x02;                        /* TDRE always 1 */
    if (!tape->motor_on) s |= 0x04;  /* DCD: 1=no carrier (motor off) */
    /* IRQ = RDRF & irq_enabled */
    if (tape->rx_full && tape->irq_enabled) s |= 0x80;
    return s;
}

/* -------------------------------------------------------------------------
 * bbc_tape_read
 *   reg=0 → status register ($FE08)
 *   reg=1 → receive data  ($FE09)
 * ------------------------------------------------------------------------- */
uint8_t bbc_tape_read(bbc_tape_t *tape, uint8_t reg)
{
    if (reg == 0) {
        return acia_status(tape);
    } else {
        /* Reading data clears RDRF */
        uint8_t d = tape->rx_data;
        tape->rx_full = false;
        /* Deassert IRQ */
        if (tape->irq_cb) tape->irq_cb(tape->irq_ctx, false);
        TAPE_LOGD("RX byte=%02X", d);
        return d;
    }
}

/* -------------------------------------------------------------------------
 * bbc_tape_write
 *   reg=0 → control register ($FE08)
 *   reg=1 → TX data ($FE09, ignored for tape load)
 * ------------------------------------------------------------------------- */
void bbc_tape_write(bbc_tape_t *tape, uint8_t reg, uint8_t val)
{
    if (reg == 0) {
        tape->acia_control = val;
        /* bits 7-6 = RX interrupt control: 10 = RX IRQ enabled */
        tape->irq_enabled = ((val >> 6) & 0x03) == 0x02 ? true :
                            ((val >> 6) & 0x03) == 0x01 ? false : false;
        /* Master reset: bits 1-0 = 11 */
        if ((val & 0x03) == 0x03) {
            tape->rx_full = false;
            if (tape->irq_cb) tape->irq_cb(tape->irq_ctx, false);
        }
        TAPE_LOGD("control=%02X irq_en=%d", val, tape->irq_enabled);
    }
    /* TX writes ignored */
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
    if (!tape->motor_on) return;
    if (!tape->blocks || tape->cur_block >= tape->n_blocks) return;
    if (tape->rx_full) return;  /* MOS hasn't read the previous byte yet */

    tape->cycle_acc += cycles;
    if (tape->cycle_acc < CYCLES_PER_BYTE) return;
    tape->cycle_acc -= CYCLES_PER_BYTE;

    /* Deliver next byte */
    bbc_tape_block_t *blk = &tape->blocks[tape->cur_block];
    tape->rx_data = blk->data[tape->cur_pos++];
    tape->rx_full = true;

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
