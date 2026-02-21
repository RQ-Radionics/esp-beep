/*
 * bbc_uservia.c — BBC Micro User VIA (IC69) for ESP32/ESP-IDF
 *
 * Thin wrapper over m6522_t. Forwards port_out/port_in/irq to the
 * host via the bbc_uservia_callbacks_t interface.
 *
 * Licence: zlib
 */

#include <string.h>
#include "bbc_uservia.h"

#ifdef ESP_PLATFORM
#  include "esp_log.h"
#  define UV_LOGD(fmt, ...) ESP_LOGD("uservia", fmt, ##__VA_ARGS__)
#else
#  define UV_LOGD(fmt, ...) /* no-op */
#endif

/* -------------------------------------------------------------------------
 * m6522 generic callbacks
 * ------------------------------------------------------------------------- */

static void uservia_port_out(void *user_ctx, uint8_t port, uint8_t val, uint8_t ddr)
{
    bbc_uservia_t *uv = (bbc_uservia_t *)user_ctx;
    UV_LOGD("port %c out %02X ddr=%02X", port ? 'B' : 'A', val, ddr);
    if (uv->cb.port_out)
        uv->cb.port_out(uv->cb.user_ctx, port, val, ddr);
}

static uint8_t uservia_port_in(void *user_ctx, uint8_t port)
{
    bbc_uservia_t *uv = (bbc_uservia_t *)user_ctx;
    if (uv->cb.port_in)
        return uv->cb.port_in(uv->cb.user_ctx, port);
    return 0xFF;
}

static void uservia_irq(void *user_ctx, bool state)
{
    bbc_uservia_t *uv = (bbc_uservia_t *)user_ctx;
    UV_LOGD("IRQ %s", state ? "assert" : "clear");
    if (uv->cb.irq)
        uv->cb.irq(uv->cb.user_ctx, state);
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void bbc_uservia_init(bbc_uservia_t *uv, const bbc_uservia_callbacks_t *callbacks)
{
    memset(uv, 0, sizeof(*uv));
    if (callbacks)
        uv->cb = *callbacks;

    m6522_callbacks_t via_cb = {
        .port_out    = uservia_port_out,
        .port_in     = uservia_port_in,
        .irq         = uservia_irq,
        .control_out = NULL,
        .user_ctx    = uv,
    };
    m6522_init(&uv->via, &via_cb);
    bbc_uservia_reset(uv);
}

void bbc_uservia_reset(bbc_uservia_t *uv)
{
    m6522_reset(&uv->via);
}

uint8_t bbc_uservia_read(bbc_uservia_t *uv, uint8_t reg)
{
    return m6522_read(&uv->via, reg);
}

void bbc_uservia_write(bbc_uservia_t *uv, uint8_t reg, uint8_t val)
{
    m6522_write(&uv->via, reg, val);
}

void bbc_uservia_tick(bbc_uservia_t *uv, int32_t cycles)
{
    m6522_tick(&uv->via, cycles);
}

void bbc_uservia_printer_ack(bbc_uservia_t *uv, bool state)
{
    /* Printer ACK strobe → CB1 (active low on real hardware) */
    m6522_set_cb1(&uv->via, state);
}
