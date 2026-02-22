/*
 * bbc_tape.h — BBC Micro tape interface emulation
 *
 * Emulates:
 *   - UEF file parser (chunks 0x0100 data blocks, 0x0110 carrier, 0x0120 gap)
 *   - Serial ULA / ACIA (MC6850-compatible) at $FE08-$FE09
 *   - Tape motor control via callback
 *
 * The MOS uses the ACIA as follows:
 *   $FE08 write = control register  (master reset, word select, Tx/Rx control)
 *   $FE08 read  = status register   (RDRF, TDRE, DCD, CTS, FE, OVRN, PE, IRQ)
 *   $FE09 read  = receive data register
 *   $FE09 write = transmit data register (ignored for tape load)
 *
 * Status bits (read $FE08):
 *   bit 0 = RDRF  Receive Data Register Full  (1 = byte ready)
 *   bit 1 = TDRE  Transmit Data Register Empty (always 1 for us)
 *   bit 2 = DCD   Data Carrier Detect          (0 = carrier present = motor on)
 *   bit 3 = CTS   Clear To Send                (0 = ok)
 *   bit 7 = IRQ   Interrupt Request
 *
 * The MOS polls RDRF and reads $FE09 for each byte.
 * It also checks DCD: if motor off (no carrier), DCD=1 → error.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * UEF chunk IDs we handle
 * ------------------------------------------------------------------------- */
#define UEF_CHUNK_ORIGIN      0x0000
#define UEF_CHUNK_DATA        0x0100  /* tape data block */
#define UEF_CHUNK_CARRIER     0x0110  /* carrier tone */
#define UEF_CHUNK_GAP_INT     0x0120  /* integer gap */
#define UEF_CHUNK_GAP_FLOAT   0x0116  /* float gap */
#define UEF_CHUNK_BAUD        0x0113  /* baud rate */

/* -------------------------------------------------------------------------
 * Tape block (decoded from UEF chunk 0x0100)
 * ------------------------------------------------------------------------- */
#define BBC_TAPE_BLOCK_MAX  512   /* max bytes in one tape block */

typedef struct {
    uint8_t  data[BBC_TAPE_BLOCK_MAX];
    uint16_t len;
} bbc_tape_block_t;

/* -------------------------------------------------------------------------
 * Tape player state
 * ------------------------------------------------------------------------- */
typedef struct {
    /* UEF data — loaded into RAM */
    uint8_t  *uef_data;
    size_t    uef_size;

    /* Block list (pointers into uef_data) */
    bbc_tape_block_t *blocks;
    uint16_t          n_blocks;
    uint16_t          cur_block;   /* block being read */
    uint16_t          cur_pos;     /* byte position within block */

    /* Motor state */
    bool motor_on;
    bool running;    /* true once motor has been turned on at least once */

    /* ACIA state */
    uint8_t  acia_control;   /* last written control byte */
    uint8_t  rx_data;        /* current receive byte */
    bool     rx_full;        /* RDRF: byte ready to read */
    bool     irq_enabled;    /* Rx IRQ enabled */

    /* Timing accumulator for byte delivery */
    int32_t cycle_acc;

    /* IRQ callback */
    void (*irq_cb)(void *ctx, bool state);
    void *irq_ctx;
} bbc_tape_t;

/* -------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/* Initialise tape state (zero everything) */
void bbc_tape_init(bbc_tape_t *tape);

/* Load a UEF file into the tape player. Returns 0 on success, -1 on error. */
int bbc_tape_load_uef(bbc_tape_t *tape, const char *path);

/* Free UEF data */
void bbc_tape_free(bbc_tape_t *tape);

/* Set IRQ callback (called when ACIA asserts IRQ) */
void bbc_tape_set_irq_cb(bbc_tape_t *tape,
                          void (*cb)(void *ctx, bool state), void *ctx);

/* Motor control — called by System VIA CB2 handler */
void bbc_tape_set_motor(bbc_tape_t *tape, bool on);

/* ACIA register access — called from bbc_machine IO handlers */
uint8_t bbc_tape_read(bbc_tape_t *tape, uint8_t reg);   /* reg 0=status/ctrl, 1=data */
void    bbc_tape_write(bbc_tape_t *tape, uint8_t reg, uint8_t val);

/* Advance tape by 'cycles' 2MHz clock cycles.
 * Delivers the next byte to the ACIA when enough time has elapsed. */
void bbc_tape_tick(bbc_tape_t *tape, int cycles);
