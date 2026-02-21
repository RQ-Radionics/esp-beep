/*
 * m6522.c — MOS 6522 VIA emulation for ESP32/ESP-IDF
 *
 * Based on floooh/chips m6522.h (zlib licence) by Andre Weissflog.
 * Pin-bus model replaced by direct register/callback API.
 * Timer pipeline model retained from floooh for accuracy.
 *
 * Licence: zlib
 * Copyright (c) 2018 Andre Weissflog
 * Adaptation (c) 2026 esp-beep project
 *
 * Key implementation notes (from floooh + 6502.org forum):
 *   - T1 always reloads from latch on underflow (both oneshot and continuous).
 *   - T2 NEVER reloads; it wraps through 0xFFFF.
 *   - Both timers decrement with a 2-cycle pipeline delay after write.
 *   - IRQ assertion is delayed one cycle after IFR becomes non-zero.
 *   - Reading T1CL clears the T1 IFR flag; reading T2CL clears T2 flag.
 *   - Writing T1CH loads counter AND clears T1 flag.
 *   - Writing T2CH loads counter AND clears T2 flag.
 */

#include <string.h>
#include "m6522.h"

#ifdef ESP_PLATFORM
#  include "esp_log.h"
#  define VIA_LOGD(fmt, ...) ESP_LOGD("m6522", fmt, ##__VA_ARGS__)
#  define VIA_LOGW(fmt, ...) ESP_LOGW("m6522", fmt, ##__VA_ARGS__)
#else
#  include <stdio.h>
#  define VIA_LOGD(fmt, ...) /* no-op */
#  define VIA_LOGW(fmt, ...) fprintf(stderr, "m6522 WARN: " fmt "\n", ##__VA_ARGS__)
/* Uncomment to enable ORA read tracing for keyboard debug */
/* #define VIA_TRACE_ORA */
#endif

/* --------------------------------------------------------------------------
 * Pipeline helpers (matches floooh bit positions)
 * COUNT pipeline: bits 0-7  (pos 2 = just written, pos 0 = active)
 * LOAD  pipeline: bits 8-15 (pos 9 = just triggered, pos 8 = fire)
 * -------------------------------------------------------------------------- */
#define PIP_COUNT_OFFSET  0
#define PIP_LOAD_OFFSET   8
#define PIP_SET(pip, off, pos)  ((pip) |=  (1u << ((off) + (pos))))
#define PIP_CLR(pip, off, pos)  ((pip) &= ~(1u << ((off) + (pos))))
#define PIP_TEST(pip, off, pos) (0 != ((pip) & (1u << ((off) + (pos)))))
#define PIP_RESET(pip, off)     ((pip) &= ~(0xFFu << (off)))

/* --------------------------------------------------------------------------
 * Internal: update IRQ output, fire callback if changed
 * -------------------------------------------------------------------------- */
static void update_irq(m6522_t *via)
{
    bool active = (via->ifr & via->ier & 0x7F) != 0;
    if (active)
        via->ifr |= M6522_IRQ_ANY;
    else
        via->ifr &= ~M6522_IRQ_ANY;

    if (active != via->irq_out) {
        via->irq_out = active;
        if (via->cb.irq)
            via->cb.irq(via->cb.user_ctx, active);
    }
}

static void set_ifr(m6522_t *via, uint8_t bits)
{
    via->ifr |= bits;
}

static void clear_ifr(m6522_t *via, uint8_t bits)
{
    via->ifr &= ~bits;
    /* Also clear the IRQ pipeline if no enabled interrupts remain */
    if ((via->ifr & via->ier & 0x7F) == 0) {
        via->ifr   &= ~M6522_IRQ_ANY;
        via->irq_pip = 0;
    }
}

/* Clear PA flags on ORA read/write (with handshake) */
static void clear_pa_intr(m6522_t *via)
{
    uint8_t mask = M6522_IRQ_CA1;
    if (!M6522_PCR_CA2_IND_IRQ(via))
        mask |= M6522_IRQ_CA2;
    clear_ifr(via, mask);
}

/* Clear PB flags on ORB read/write */
static void clear_pb_intr(m6522_t *via)
{
    uint8_t mask = M6522_IRQ_CB1;
    if (!M6522_PCR_CB2_IND_IRQ(via))
        mask |= M6522_IRQ_CB2;
    clear_ifr(via, mask);
}

/* --------------------------------------------------------------------------
 * Internal: combined output pin value for a port
 * -------------------------------------------------------------------------- */
static uint8_t port_pins(const m6522_port_t *p)
{
    return (p->outr & p->ddr) | (p->inpr & ~p->ddr);
}

static uint8_t port_pins_pb7(const m6522_t *via)
{
    uint8_t val = port_pins(&via->pb);
    if (via->acr & M6522_ACR_T1_PB7) {
        val &= ~0x80;
        if (via->t1.t_bit)
            val |= 0x80;
    }
    return val;
}

/* Notify host of port output changes */
static void notify_port_out(m6522_t *via, uint8_t port)
{
    if (!via->cb.port_out) return;
    if (port == 0) {
        uint8_t val = port_pins(&via->pa);
        via->cb.port_out(via->cb.user_ctx, 0, val, via->pa.ddr);
    } else {
        uint8_t val = port_pins_pb7(via);
        via->cb.port_out(via->cb.user_ctx, 1, val, via->pb.ddr);
    }
}

/* --------------------------------------------------------------------------
 * Timer tick (one 1 MHz cycle)
 * -------------------------------------------------------------------------- */
static void tick_t1(m6522_t *via)
{
    m6522_timer_t *t = &via->t1;

    /* decrement if pipeline active */
    if (PIP_TEST(t->pip, PIP_COUNT_OFFSET, 0))
        t->counter--;

    /* underflow detection: counter wraps through 0xFFFF */
    t->t_out = (t->counter == 0xFFFF);
    if (t->t_out) {
        if (via->acr & M6522_ACR_T1_CONTINUOUS) {
            t->t_bit = !t->t_bit;
            set_ifr(via, M6522_IRQ_T1);
        } else {
            if (!t->t_bit) {
                set_ifr(via, M6522_IRQ_T1);
                t->t_bit = true;
            }
        }
        /* T1 always reloads from latch */
        PIP_SET(t->pip, PIP_LOAD_OFFSET, 1);
    }

    /* reload from latch */
    if (PIP_TEST(t->pip, PIP_LOAD_OFFSET, 0))
        t->counter = t->latch;
}

static void tick_t2(m6522_t *via, bool pb6_edge)
{
    m6522_timer_t *t = &via->t2;

    if (via->acr & M6522_ACR_T2_COUNT_PB6) {
        /* count falling edge of PB6 */
        if (pb6_edge)
            t->counter--;
    } else if (PIP_TEST(t->pip, PIP_COUNT_OFFSET, 0)) {
        t->counter--;
    }

    t->t_out = (t->counter == 0xFFFF);
    if (t->t_out) {
        if (!t->t_bit) {
            set_ifr(via, M6522_IRQ_T2);
            t->t_bit = true;
        }
        /* T2 never reloads */
    }
}

/* --------------------------------------------------------------------------
 * Edge detection on control lines
 * -------------------------------------------------------------------------- */
static void update_control_lines(m6522_t *via)
{
    /* CA1 */
    if (via->pa.c1_triggered) {
        set_ifr(via, M6522_IRQ_CA1);
        if (M6522_PCR_CA2_AUTO_HS(via))
            via->pa.c2_out = true;
    }
    /* CA2 in input mode */
    if (via->pa.c2_triggered && M6522_PCR_CA2_INPUT(via))
        set_ifr(via, M6522_IRQ_CA2);

    /* CB1 */
    if (via->pb.c1_triggered) {
        set_ifr(via, M6522_IRQ_CB1);
        if (M6522_PCR_CB2_AUTO_HS(via))
            via->pb.c2_out = true;
    }
    /* CB2 in input mode */
    if (via->pb.c2_triggered && M6522_PCR_CB2_INPUT(via))
        set_ifr(via, M6522_IRQ_CB2);

    /* Clear triggered flags */
    via->pa.c1_triggered = false;
    via->pa.c2_triggered = false;
    via->pb.c1_triggered = false;
    via->pb.c2_triggered = false;
}

/* Advance pipelines one step */
static void advance_pipelines(m6522_t *via)
{
    PIP_SET(via->t1.pip, PIP_COUNT_OFFSET, 2);
    PIP_SET(via->t2.pip, PIP_COUNT_OFFSET, 2);

    if (via->ifr & via->ier & 0x7F)
        via->irq_pip |= 0x02;

    via->t1.pip  = (via->t1.pip  >> 1) & 0x7F7F;
    via->t2.pip  = (via->t2.pip  >> 1) & 0x7F7F;
    via->irq_pip = (via->irq_pip >> 1) & 0x7F;
}

/* --------------------------------------------------------------------------
 * Register read
 * -------------------------------------------------------------------------- */
static uint8_t reg_read(m6522_t *via, uint8_t addr)
{
    uint8_t data = 0;
    switch (addr & 0x0F) {
        case M6522_REG_ORB:
            if (via->acr & M6522_ACR_PB_LATCH)
                data = via->pb.inpr;
            else
                data = port_pins_pb7(via);
            clear_pb_intr(via);
            VIA_LOGD("read ORB -> %02X", data);
            break;

        case M6522_REG_ORA:
            if (via->acr & M6522_ACR_PA_LATCH)
                data = via->pa.inpr;
            else {
                /* refresh input from callback */
                if (via->cb.port_in)
                    via->pa.inpr = via->cb.port_in(via->cb.user_ctx, 0);
                data = port_pins(&via->pa);
            }
            clear_pa_intr(via);
            if (M6522_PCR_CA2_AUTO_HS(via) || M6522_PCR_CA2_PULSE_OUTPUT(via))
                via->pa.c2_out = false;
#ifdef VIA_TRACE_ORA
            fprintf(stderr, "[ORA_read] outr=%02X ddr=%02X inpr=%02X data=%02X IFR=%02X IER=%02X\n",
                    via->pa.outr, via->pa.ddr, via->pa.inpr, data, via->ifr, via->ier);
#endif
            VIA_LOGD("read ORA -> %02X", data);
            break;

        case M6522_REG_DDRB:
            data = via->pb.ddr;
            break;

        case M6522_REG_DDRA:
            data = via->pa.ddr;
            break;

        case M6522_REG_T1CL:
            data = via->t1.counter & 0xFF;
            clear_ifr(via, M6522_IRQ_T1);
            break;

        case M6522_REG_T1CH:
            data = via->t1.counter >> 8;
            break;

        case M6522_REG_T1LL:
            data = via->t1.latch & 0xFF;
            break;

        case M6522_REG_T1LH:
            data = via->t1.latch >> 8;
            break;

        case M6522_REG_T2CL:
            data = via->t2.counter & 0xFF;
            clear_ifr(via, M6522_IRQ_T2);
            break;

        case M6522_REG_T2CH:
            data = via->t2.counter >> 8;
            break;

        case M6522_REG_SR:
            data = via->sr;
            /* reading SR clears SR interrupt */
            clear_ifr(via, M6522_IRQ_SR);
            break;

        case M6522_REG_ACR:
            data = via->acr;
            break;

        case M6522_REG_PCR:
            data = via->pcr;
            break;

        case M6522_REG_IFR:
            data = via->ifr;
            break;

        case M6522_REG_IER:
            data = via->ier | 0x80;  /* bit 7 always reads as 1 */
            break;

        case M6522_REG_ORA_NH:
            if (via->acr & M6522_ACR_PA_LATCH)
                data = via->pa.inpr;
            else {
                if (via->cb.port_in)
                    via->pa.inpr = via->cb.port_in(via->cb.user_ctx, 0);
                data = port_pins(&via->pa);
            }
            /* NOTE: no handshake clear */
#ifdef VIA_TRACE_ORA
            fprintf(stderr, "[ORA_NH_read] outr=%02X ddr=%02X inpr=%02X data=%02X IFR=%02X IER=%02X\n",
                    via->pa.outr, via->pa.ddr, via->pa.inpr, data, via->ifr, via->ier);
#endif
            break;
    }
    return data;
}

/* --------------------------------------------------------------------------
 * Register write
 * -------------------------------------------------------------------------- */
static void reg_write(m6522_t *via, uint8_t addr, uint8_t data)
{
    switch (addr & 0x0F) {
        case M6522_REG_ORB:
            via->pb.outr = data;
            clear_pb_intr(via);
            if (M6522_PCR_CB2_AUTO_HS(via))
                via->pb.c2_out = false;
            notify_port_out(via, 1);
            VIA_LOGD("write ORB %02X", data);
            break;

        case M6522_REG_ORA:
            via->pa.outr = data;
            clear_pa_intr(via);
            if (M6522_PCR_CA2_AUTO_HS(via) || M6522_PCR_CA2_PULSE_OUTPUT(via))
                via->pa.c2_out = false;
            notify_port_out(via, 0);
            VIA_LOGD("write ORA %02X", data);
            break;

        case M6522_REG_DDRB:
            via->pb.ddr = data;
            notify_port_out(via, 1);
            break;

        case M6522_REG_DDRA:
            via->pa.ddr = data;
            notify_port_out(via, 0);
            break;

        case M6522_REG_T1CL:
        case M6522_REG_T1LL:
            via->t1.latch = (via->t1.latch & 0xFF00) | data;
            break;

        case M6522_REG_T1CH:
            via->t1.latch   = (via->t1.latch & 0x00FF) | ((uint16_t)data << 8);
            via->t1.counter = via->t1.latch;
            /* In PB7 mode t_bit drives PB7 output and must NOT be reset on
             * re-arm — PB7 only changes on underflow.  Outside PB7 mode the
             * bit is an internal one-shot guard; reset it so one-shot fires. */
            if (!(via->acr & M6522_ACR_T1_PB7))
                via->t1.t_bit = false;
            clear_ifr(via, M6522_IRQ_T1);
            PIP_RESET(via->t1.pip, PIP_COUNT_OFFSET);
            PIP_SET(via->t1.pip, PIP_COUNT_OFFSET, 2);
            VIA_LOGD("write T1CH %02X, latch=%04X", data, via->t1.latch);
            break;

        case M6522_REG_T1LH:
            via->t1.latch = (via->t1.latch & 0x00FF) | ((uint16_t)data << 8);
            clear_ifr(via, M6522_IRQ_T1);
            break;

        case M6522_REG_T2CL:
            via->t2.latch = (via->t2.latch & 0xFF00) | data;
            break;

        case M6522_REG_T2CH:
            via->t2.latch   = (via->t2.latch & 0x00FF) | ((uint16_t)data << 8);
            via->t2.counter = via->t2.latch;
            via->t2.t_bit   = false;
            clear_ifr(via, M6522_IRQ_T2);
            PIP_RESET(via->t2.pip, PIP_COUNT_OFFSET);
            PIP_SET(via->t2.pip, PIP_COUNT_OFFSET, 2);
            VIA_LOGD("write T2CH %02X, latch=%04X", data, via->t2.latch);
            break;

        case M6522_REG_SR:
            via->sr = data;
            clear_ifr(via, M6522_IRQ_SR);
            break;

        case M6522_REG_ACR:
            via->acr = data;
            if (!(data & M6522_ACR_T2_COUNT_PB6))
                PIP_CLR(via->t2.pip, PIP_COUNT_OFFSET, 0);
            break;

        case M6522_REG_PCR:
            via->pcr = data;
            if (M6522_PCR_CA2_FIX_OUTPUT(via)) {
                bool lvl = M6522_PCR_CA2_OUTPUT_LEVEL(via);
                via->pa.c2_out = lvl;
                if (via->cb.control_out)
                    via->cb.control_out(via->cb.user_ctx, 0, lvl);
            }
            if (M6522_PCR_CB2_FIX_OUTPUT(via)) {
                bool lvl = M6522_PCR_CB2_OUTPUT_LEVEL(via);
                via->pb.c2_out = lvl;
                if (via->cb.control_out)
                    via->cb.control_out(via->cb.user_ctx, 1, lvl);
            }
            break;

        case M6522_REG_IFR:
            /* Writing IFR: bits set to 1 CLEAR the corresponding flags */
            if (data & M6522_IRQ_ANY)
                data = 0x7F;
            clear_ifr(via, data);
            break;

        case M6522_REG_IER:
            /* Bit 7 = 1 → set bits; bit 7 = 0 → clear bits */
            if (data & 0x80)
                via->ier |=  (data & 0x7F);
            else
                via->ier &= ~(data & 0x7F);
            break;

        case M6522_REG_ORA_NH:
            via->pa.outr = data;
            notify_port_out(via, 0);
            break;
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void m6522_init(m6522_t *via, const m6522_callbacks_t *callbacks)
{
    memset(via, 0, sizeof(*via));
    if (callbacks)
        via->cb = *callbacks;

    /* Set initial latch values to 0xFFFF per datasheet */
    via->t1.latch = 0xFFFF;
    via->t2.latch = 0xFFFF;
    /* Input registers default high (no pull-downs) */
    via->pa.inpr  = 0xFF;
    via->pb.inpr  = 0xFF;
    via->pa.c1_out = true;
    via->pa.c2_out = true;
    via->pb.c1_out = true;
    via->pb.c2_out = true;

    m6522_reset(via);
}

void m6522_reset(m6522_t *via)
{
    /* "RESET clears all internal registers to logic 0, except T1, T2, SR" */
    via->pa.outr  = 0;
    via->pa.ddr   = 0;
    via->pb.outr  = 0;
    via->pb.ddr   = 0;
    via->pa.c1_triggered = false;
    via->pa.c2_triggered = false;
    via->pb.c1_triggered = false;
    via->pb.c2_triggered = false;
    via->pa.c1_out = true;
    via->pa.c2_out = true;
    via->pb.c1_out = true;
    via->pb.c2_out = true;
    via->acr      = 0;
    via->pcr      = 0;
    via->ifr      = 0;
    via->ier      = 0;
    via->irq_out  = false;
    via->irq_pip  = 0;
    via->t1.pip   = 0;
    via->t1.t_bit = false;
    via->t1.t_out = false;
    via->t2.pip   = 0;
    via->t2.t_bit = false;
    via->t2.t_out = false;
    VIA_LOGD("reset");
}

uint8_t m6522_read(m6522_t *via, uint8_t reg)
{
    uint8_t val = reg_read(via, reg);
    update_irq(via);
    return val;
}

void m6522_write(m6522_t *via, uint8_t reg, uint8_t val)
{
    reg_write(via, reg, val);
    update_irq(via);
}

void m6522_tick(m6522_t *via, int32_t cycles)
{
    /* Track PB6 state for T2 pulse counting */
    uint8_t pb_old = port_pins_pb7(via);

    for (int32_t i = 0; i < cycles; i++) {
        /* Refresh port A input from callback (live mode, no latch) */
        if (!(via->acr & M6522_ACR_PA_LATCH) && via->cb.port_in)
            via->pa.inpr = via->cb.port_in(via->cb.user_ctx, 0);

        /* Control line edge detection */
        update_control_lines(via);

        /* PB6 falling edge detection for T2 */
        uint8_t pb_now = port_pins_pb7(via);
        bool pb6_edge  = (pb_old & 0x40) && !(pb_now & 0x40);
        pb_old         = pb_now;

        tick_t1(via);
        tick_t2(via, pb6_edge);

        /* IRQ pipeline: set bit 7 of IFR after 1-cycle delay */
        if (via->irq_pip & 0x01) {
            via->ifr |= M6522_IRQ_ANY;
            if (!via->irq_out) {
                via->irq_out = true;
                if (via->cb.irq)
                    via->cb.irq(via->cb.user_ctx, true);
            }
        }

        advance_pipelines(via);
    }

    update_irq(via);
}

void m6522_set_ca1(m6522_t *via, bool state)
{
    bool prev = via->pa.c1_in;
    via->pa.c1_in = state;
    via->pa.c1_triggered =
        (prev != state) &&
        ((state && M6522_PCR_CA1_LOW_TO_HIGH(via)) ||
         (!state && M6522_PCR_CA1_HIGH_TO_LOW(via)));
}

void m6522_set_ca2(m6522_t *via, bool state)
{
    bool prev = via->pa.c2_in;
    via->pa.c2_in = state;
    if (M6522_PCR_CA2_INPUT(via)) {
        via->pa.c2_triggered =
            (prev != state) &&
            ((state && M6522_PCR_CA2_LOW_TO_HIGH(via)) ||
             (!state && M6522_PCR_CA2_HIGH_TO_LOW(via)));
    }
}

void m6522_set_cb1(m6522_t *via, bool state)
{
    bool prev = via->pb.c1_in;
    via->pb.c1_in = state;
    via->pb.c1_triggered =
        (prev != state) &&
        ((state && M6522_PCR_CB1_LOW_TO_HIGH(via)) ||
         (!state && M6522_PCR_CB1_HIGH_TO_LOW(via)));
}

void m6522_set_cb2(m6522_t *via, bool state)
{
    bool prev = via->pb.c2_in;
    via->pb.c2_in = state;
    if (M6522_PCR_CB2_INPUT(via)) {
        via->pb.c2_triggered =
            (prev != state) &&
            ((state && M6522_PCR_CB2_LOW_TO_HIGH(via)) ||
             (!state && M6522_PCR_CB2_HIGH_TO_LOW(via)));
    }
}

void m6522_set_port_a(m6522_t *via, uint8_t val)
{
    /* Only update bits configured as inputs */
    via->pa.inpr = (via->pa.inpr & via->pa.ddr) | (val & ~via->pa.ddr);
}

void m6522_set_port_b(m6522_t *via, uint8_t val)
{
    via->pb.inpr = (via->pb.inpr & via->pb.ddr) | (val & ~via->pb.ddr);
}

bool m6522_get_irq(const m6522_t *via)
{
    return via->irq_out;
}

uint8_t m6522_get_port_a(const m6522_t *via)
{
    return port_pins(&via->pa);
}

uint8_t m6522_get_port_b(const m6522_t *via)
{
    return port_pins_pb7(via);
}
