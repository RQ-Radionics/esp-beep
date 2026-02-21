# m6522 — MOS 6522 VIA component for ESP32/ESP-IDF

Generic MOS 6522 VIA emulation plus BBC Micro-specific System VIA and User VIA
layers, adapted from [floooh/chips](https://github.com/floooh/chips) (zlib) and
[B-em](https://github.com/stardot/b-em) (GPL-2.0).

## Files

| File | Description |
|---|---|
| `include/m6522.h` | Generic 6522 API — struct, register constants, callbacks |
| `m6522.c` | Generic 6522 implementation (timers, ports, edge detection, IRQ) |
| `include/bbc_sysvia.h` | System VIA BBC-specific layer |
| `bbc_sysvia.c` | IC32 addressable latch, keyboard, sound, VSYNC, joystick |
| `include/bbc_uservia.h` | User VIA BBC-specific layer |
| `bbc_uservia.c` | Pass-through with printer ACK and user port callbacks |

## BBC Micro VIA register map

### System VIA (IC3) — &FE40–&FE4F

| Offset | Read | Write |
|--------|------|-------|
| &FE40 | IRB / input reg B | ORB (→ IC32 latch via PB0-PB3) |
| &FE41 | IRA / slow data bus | ORA (→ keyboard col / SN76489) |
| &FE42 | DDRB | DDRB |
| &FE43 | DDRA | DDRA |
| &FE44 | T1CL (clears T1 IRQ) | T1CL latch |
| &FE45 | T1CH | T1CH (loads + starts timer) |
| &FE46 | T1LL | T1LL |
| &FE47 | T1LH | T1LH |
| &FE48 | T2CL (clears T2 IRQ) | T2CL latch |
| &FE49 | T2CH | T2CH (loads + starts timer) |
| &FE4A | SR | SR |
| &FE4B | ACR | ACR |
| &FE4C | PCR | PCR |
| &FE4D | IFR | IFR (write 1 to clear bits) |
| &FE4E | IER \| 0x80 | IER (bit 7: 1=set, 0=clear) |
| &FE4F | IRA (no handshake) | ORA (no handshake) |

### User VIA (IC69) — &FE60–&FE6F

Same register layout, same offsets relative to &FE60.

## Addressable latch IC32 (System VIA Port B)

| Bit | Name | Function |
|-----|------|----------|
| 0 | SOUND_WE | SN76489 write enable (active LOW) |
| 1 | SPEECH_RD | Speech chip read (stub) |
| 2 | SPEECH_WR | Speech chip write (stub) |
| 3 | KB_AUTOSCAN | Keyboard auto-scan enable |
| 4 | SCREEN_B0 | Screen bank select bit 0 (Master 128) |
| 5 | SCREEN_B1 | Screen bank select bit 1 (Master 128) |
| 6 | CAPS_LED | Caps Lock LED (active LOW) |
| 7 | SHIFT_LED | Shift Lock LED (active LOW) |

## Key signal routing

```
6845 CRTC VSYNC  → bbc_sysvia_vsync()    → CA1 → IFR bit 1 (CA1) → IRQ
ADC EOC          → bbc_sysvia_adc_eoc()  → CB1 → IFR bit 4 (CB1) → IRQ
System VIA IRQ   → bbc_sysvia cb.irq()   → 6502 /IRQ
User   VIA IRQ   → bbc_uservia cb.irq()  → 6502 /IRQ (OR'd)
WD1770 INTRQ     → wd1770 cb.irq()       → 6502 /NMI
```

## Usage

```c
#include "bbc_sysvia.h"
#include "bbc_uservia.h"

static bbc_sysvia_t  sysvia;
static bbc_uservia_t uservia;

/* --- System VIA setup --- */
static void my_sound_write(void *ctx, uint8_t data) {
    sn76489_write(data);
}
static uint8_t my_kbd_read(void *ctx, uint8_t col) {
    return keyboard_scan_column(col);
}
static void my_sysvia_irq(void *ctx, bool state) {
    cpu_set_irq(state);
}

bbc_sysvia_callbacks_t sv_cb = {
    .sound_write   = my_sound_write,
    .keyboard_read = my_kbd_read,
    .latch_changed = NULL,   /* or implement LED/screen updates */
    .irq           = my_sysvia_irq,
    .user_ctx      = NULL,
};
bbc_sysvia_init(&sysvia, &sv_cb);

/* --- User VIA setup (minimal) --- */
bbc_uservia_callbacks_t uv_cb = {
    .port_out = NULL,
    .port_in  = NULL,
    .irq      = my_sysvia_irq,  /* OR'd with system VIA IRQ */
    .user_ctx = NULL,
};
bbc_uservia_init(&uservia, &uv_cb);

/* --- Per-cycle tick (call once per 1 MHz BBC clock cycle) --- */
void bbc_tick(void) {
    bbc_sysvia_tick(&sysvia, 1);
    bbc_uservia_tick(&uservia, 1);
}

/* --- VSYNC from 6845 --- */
void bbc_vsync(bool state) {
    bbc_sysvia_vsync(&sysvia, state);
}

/* --- Memory map reads/writes --- */
uint8_t bbc_read_sysvia(uint16_t addr) {
    return bbc_sysvia_read(&sysvia, addr & 0x0F);
}
void bbc_write_sysvia(uint16_t addr, uint8_t val) {
    bbc_sysvia_write(&sysvia, addr & 0x0F, val);
}
```

## Integration with WD1770 disk controller

The System VIA Port B controls the WD1770 via the IC32 latch and direct bits.
On the standard Acorn WD1770 interface (BBC B+/Master):

```c
static void latch_changed(void *ctx, uint8_t latch) {
    /* Drive select, side, density come from Port B bits written to the
       WD1770 control latch at &FE80 (separate from IC32) */
    (void)latch;
}
```

The WD1770 INTRQ is connected to the CPU NMI line directly, not through the VIA.

## References

- MOS 6522 datasheet: https://www.westerndesigncenter.com/wdc/documentation/w65c22.pdf
- BBC Micro Advanced User Guide, Chapter 23 (System VIA)
- floooh/chips: https://github.com/floooh/chips/blob/master/chips/m6522.h
- B-em sysvia.c: https://github.com/stardot/b-em/blob/master/src/sysvia.c
- Timer behaviour: http://forum.6502.org/viewtopic.php?f=4&t=2901
