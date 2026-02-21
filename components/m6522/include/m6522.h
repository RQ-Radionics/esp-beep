/*
 * m6522.h — MOS 6522 VIA emulation for ESP32/ESP-IDF
 *
 * Derived from floooh/chips m6522.h (zlib licence) by Andre Weissflog.
 * Adapted for ESP-IDF: pin-bus model replaced by a direct register/callback
 * API. All state in m6522_t — no globals, no malloc.
 *
 * Licence: zlib (same as original)
 * Copyright (c) 2018 Andre Weissflog
 * Adaptation (c) 2026 esp-beep project
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Register offsets (pass to m6522_read / m6522_write as reg 0x0-0xF)
 * -------------------------------------------------------------------------- */
#define M6522_REG_ORB       0x0   /* Output/Input Register B                 */
#define M6522_REG_ORA       0x1   /* Output/Input Register A (with handshake)*/
#define M6522_REG_DDRB      0x2   /* Data Direction Register B               */
#define M6522_REG_DDRA      0x3   /* Data Direction Register A               */
#define M6522_REG_T1CL      0x4   /* Timer 1 Counter Low (read) / Latch Low  */
#define M6522_REG_T1CH      0x5   /* Timer 1 Counter High                    */
#define M6522_REG_T1LL      0x6   /* Timer 1 Latch Low                       */
#define M6522_REG_T1LH      0x7   /* Timer 1 Latch High                      */
#define M6522_REG_T2CL      0x8   /* Timer 2 Counter Low (read) / Latch Low  */
#define M6522_REG_T2CH      0x9   /* Timer 2 Counter High                    */
#define M6522_REG_SR        0xA   /* Shift Register                          */
#define M6522_REG_ACR       0xB   /* Auxiliary Control Register              */
#define M6522_REG_PCR       0xC   /* Peripheral Control Register             */
#define M6522_REG_IFR       0xD   /* Interrupt Flag Register                 */
#define M6522_REG_IER       0xE   /* Interrupt Enable Register               */
#define M6522_REG_ORA_NH    0xF   /* Output/Input Register A (no handshake)  */

/* --------------------------------------------------------------------------
 * IFR / IER bit masks
 * -------------------------------------------------------------------------- */
#define M6522_IRQ_CA2       0x01
#define M6522_IRQ_CA1       0x02
#define M6522_IRQ_SR        0x04
#define M6522_IRQ_CB2       0x08
#define M6522_IRQ_CB1       0x10
#define M6522_IRQ_T2        0x20
#define M6522_IRQ_T1        0x40
#define M6522_IRQ_ANY       0x80  /* Set when any enabled interrupt is active */

/* --------------------------------------------------------------------------
 * ACR bit fields
 * -------------------------------------------------------------------------- */
#define M6522_ACR_PA_LATCH      0x01  /* Port A input latch enable            */
#define M6522_ACR_PB_LATCH      0x02  /* Port B input latch enable            */
#define M6522_ACR_SR_MASK       0x1C  /* Shift register mode (bits 2-4)       */
#define M6522_ACR_T2_COUNT_PB6  0x20  /* T2: count falling edges on PB6       */
#define M6522_ACR_T1_CONTINUOUS 0x40  /* T1: continuous (free-running) mode   */
#define M6522_ACR_T1_PB7        0x80  /* T1: toggle PB7 output on underflow   */

/* --------------------------------------------------------------------------
 * PCR helper macros (from floooh/chips, MAME naming)
 * -------------------------------------------------------------------------- */
#define M6522_PCR_CA1_LOW_TO_HIGH(c)  ((c)->pcr & 0x01)
#define M6522_PCR_CA1_HIGH_TO_LOW(c)  (!((c)->pcr & 0x01))
#define M6522_PCR_CB1_LOW_TO_HIGH(c)  ((c)->pcr & 0x10)
#define M6522_PCR_CB1_HIGH_TO_LOW(c)  (!((c)->pcr & 0x10))
#define M6522_PCR_CA2_INPUT(c)        (!((c)->pcr & 0x08))
#define M6522_PCR_CA2_LOW_TO_HIGH(c)  (((c)->pcr & 0x0c) == 0x04)
#define M6522_PCR_CA2_HIGH_TO_LOW(c)  (((c)->pcr & 0x0c) == 0x00)
#define M6522_PCR_CA2_IND_IRQ(c)      (((c)->pcr & 0x0a) == 0x02)
#define M6522_PCR_CA2_OUTPUT(c)       ((c)->pcr & 0x08)
#define M6522_PCR_CA2_AUTO_HS(c)      (((c)->pcr & 0x0c) == 0x08)
#define M6522_PCR_CA2_PULSE_OUTPUT(c) (((c)->pcr & 0x0e) == 0x0a)
#define M6522_PCR_CA2_FIX_OUTPUT(c)   (((c)->pcr & 0x0c) == 0x0c)
#define M6522_PCR_CA2_OUTPUT_LEVEL(c) (((c)->pcr & 0x02) >> 1)
#define M6522_PCR_CB2_INPUT(c)        (!((c)->pcr & 0x80))
#define M6522_PCR_CB2_LOW_TO_HIGH(c)  (((c)->pcr & 0xc0) == 0x40)
#define M6522_PCR_CB2_HIGH_TO_LOW(c)  (((c)->pcr & 0xc0) == 0x00)
#define M6522_PCR_CB2_IND_IRQ(c)      (((c)->pcr & 0xa0) == 0x20)
#define M6522_PCR_CB2_OUTPUT(c)       ((c)->pcr & 0x80)
#define M6522_PCR_CB2_AUTO_HS(c)      (((c)->pcr & 0xc0) == 0x80)
#define M6522_PCR_CB2_PULSE_OUTPUT(c) (((c)->pcr & 0xe0) == 0xa0)
#define M6522_PCR_CB2_FIX_OUTPUT(c)   (((c)->pcr & 0xc0) == 0xc0)
#define M6522_PCR_CB2_OUTPUT_LEVEL(c) (((c)->pcr & 0x20) >> 5)

/* --------------------------------------------------------------------------
 * Callbacks
 * -------------------------------------------------------------------------- */
typedef struct {
    /*
     * Called when the output value of port A or B changes.
     * port: 0=A, 1=B
     * val:  current combined pin state (outr & ddr | inpr & ~ddr)
     * ddr:  direction register (1=output bit)
     */
    void (*port_out)(void *user_ctx, uint8_t port, uint8_t val, uint8_t ddr);

    /*
     * Called to read external pin state for port A or B.
     * port: 0=A, 1=B
     * Returns the byte present on the physical pins (only input bits matter).
     */
    uint8_t (*port_in)(void *user_ctx, uint8_t port);

    /*
     * Called when IRQ output changes state.
     * state=true: IRQ asserted (active low on real hardware, active high here).
     */
    void (*irq)(void *user_ctx, bool state);

    /*
     * Called when CA2 or CB2 changes state in output mode.
     * line: 0=CA2, 1=CB2
     */
    void (*control_out)(void *user_ctx, uint8_t line, bool state);

    void *user_ctx;
} m6522_callbacks_t;

/* --------------------------------------------------------------------------
 * Timer sub-state
 * -------------------------------------------------------------------------- */
typedef struct {
    uint16_t latch;    /* reload value (T2: only low byte is latched)  */
    uint16_t counter;  /* current counter value                         */
    bool     t_bit;    /* toggles on underflow (T1: PB7; T2: fired)    */
    bool     t_out;    /* true for one tick at underflow                */
    uint8_t  pip;      /* 2-cycle count pipeline + 1-cycle load pipeline */
} m6522_timer_t;

/* --------------------------------------------------------------------------
 * Port sub-state
 * -------------------------------------------------------------------------- */
typedef struct {
    uint8_t outr;         /* output register (ORA / ORB)                */
    uint8_t inpr;         /* input register (latched or live)            */
    uint8_t ddr;          /* data direction (1=output)                   */
    uint8_t pins;         /* combined pin state output to callbacks      */
    bool    c1_in;        /* current state of CA1/CB1                    */
    bool    c1_out;       /* CA1/CB1 output state (CA1 is input-only)    */
    bool    c1_triggered; /* edge detected on CA1/CB1 this tick          */
    bool    c2_in;        /* current state of CA2/CB2                    */
    bool    c2_out;       /* CA2/CB2 output state                        */
    bool    c2_triggered; /* edge detected on CA2/CB2 this tick          */
} m6522_port_t;

/* --------------------------------------------------------------------------
 * Full VIA state — allocate statically, no heap needed
 * -------------------------------------------------------------------------- */
typedef struct {
    m6522_port_t  pa;
    m6522_port_t  pb;
    m6522_timer_t t1;
    m6522_timer_t t2;

    uint8_t acr;        /* Auxiliary Control Register   */
    uint8_t pcr;        /* Peripheral Control Register  */
    uint8_t ifr;        /* Interrupt Flag Register      */
    uint8_t ier;        /* Interrupt Enable Register    */
    uint8_t sr;         /* Shift Register               */
    uint8_t sr_count;   /* bits shifted so far (0-8)    */

    bool    irq_out;    /* current IRQ output state     */

    /* interrupt pipeline (1-cycle delay before IRQ asserts) */
    uint8_t irq_pip;

    m6522_callbacks_t cb;
} m6522_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/* Initialise; callbacks may be NULL for polling-only use */
void    m6522_init(m6522_t *via, const m6522_callbacks_t *callbacks);

/* Hardware reset — clears DDR, IER, IFR, ACR, PCR, disables timers */
void    m6522_reset(m6522_t *via);

/* Register read/write (reg = 0x0-0xF) */
uint8_t m6522_read(m6522_t *via, uint8_t reg);
void    m6522_write(m6522_t *via, uint8_t reg, uint8_t val);

/*
 * Clock tick — call once per BBC Micro 1 MHz CPU cycle (or pass accumulated
 * cycles; for timing accuracy pass 1 each call from the main CPU loop).
 * Decrements timers, detects edges, updates IFR, fires IRQ callback.
 */
void    m6522_tick(m6522_t *via, int32_t cycles);

/* Set control line inputs (edge detection happens inside tick) */
void    m6522_set_ca1(m6522_t *via, bool state);
void    m6522_set_ca2(m6522_t *via, bool state);
void    m6522_set_cb1(m6522_t *via, bool state);
void    m6522_set_cb2(m6522_t *via, bool state);

/* Set external port pin state (input bits only; output bits are ignored) */
void    m6522_set_port_a(m6522_t *via, uint8_t val);
void    m6522_set_port_b(m6522_t *via, uint8_t val);

/* Poll output signals */
bool    m6522_get_irq(const m6522_t *via);
uint8_t m6522_get_port_a(const m6522_t *via);
uint8_t m6522_get_port_b(const m6522_t *via);

#ifdef __cplusplus
}
#endif
