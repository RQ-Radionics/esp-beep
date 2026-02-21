/*
 * bbc_uservia.h — BBC Micro User VIA (IC69, &FE60-&FE6F) for ESP32/ESP-IDF
 *
 * Thin wrapper over the generic m6522_t. The User VIA provides:
 *   - Port A: Printer data output (Centronics)
 *   - Port B: User port (general purpose)
 *   - CB1/CB2: Printer handshake / user port control
 *
 * Typical uses: Centronics printer, AMX mouse, user-port peripherals.
 *
 * Licence: zlib (adaptation of floooh/chips)
 */

#pragma once
#include "m6522.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Callbacks
 * -------------------------------------------------------------------------- */
typedef struct {
    /*
     * Called when the output value of port A or B changes.
     * port: 0=A (printer data), 1=B (user port)
     * val:  current output byte
     * ddr:  data direction register
     */
    void (*port_out)(void *user_ctx, uint8_t port, uint8_t val, uint8_t ddr);

    /*
     * Called to read external pin state for port A or B.
     * port: 0=A, 1=B
     */
    uint8_t (*port_in)(void *user_ctx, uint8_t port);

    /*
     * IRQ from the User VIA changed state.
     * On the BBC Micro this is OR'd with the System VIA IRQ and fed to 6502 /IRQ.
     */
    void (*irq)(void *user_ctx, bool state);

    void *user_ctx;
} bbc_uservia_callbacks_t;

/* --------------------------------------------------------------------------
 * User VIA state
 * -------------------------------------------------------------------------- */
typedef struct {
    m6522_t via;
    bbc_uservia_callbacks_t cb;
} bbc_uservia_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void    bbc_uservia_init(bbc_uservia_t *uv, const bbc_uservia_callbacks_t *callbacks);
void    bbc_uservia_reset(bbc_uservia_t *uv);

/* Register access at &FE60-&FE6F (pass addr & 0x0F as reg) */
uint8_t bbc_uservia_read(bbc_uservia_t *uv, uint8_t reg);
void    bbc_uservia_write(bbc_uservia_t *uv, uint8_t reg, uint8_t val);

/* Clock tick — call once per 1 MHz BBC CPU cycle */
void    bbc_uservia_tick(bbc_uservia_t *uv, int32_t cycles);

/* Printer ACK strobe → CB1 (active low) */
void    bbc_uservia_printer_ack(bbc_uservia_t *uv, bool state);

#ifdef __cplusplus
}
#endif
