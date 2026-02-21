/*
 * Tests for bbc_memory — BBC Micro Model B memory subsystem
 *
 * Covers:
 *   - create / destroy lifecycle
 *   - initial state (RAM=0x00, ROM=0xFF, no callbacks)
 *   - RAM read/write (0x0000–0x7FFF)
 *   - Sideways ROM (0x8000–0xBFFF): unloaded, loaded, partial, oversized, wrong addr
 *   - OS ROM (0xC000–0xFBFF): unloaded, loaded
 *   - ROM write protection (silently dropped)
 *   - I/O region (0xFC00–0xFFFF): no callback falls through to OS ROM
 *   - Read callbacks: fired with correct addr/userdata, boundaries, below IO_START ignored
 *   - Write callbacks: fired with correct addr/value/userdata
 *   - Independent read/write userdata at same address
 *   - Range callbacks: full range, boundary, rejected below IO_START
 *   - bbc_memory_get_ram: valid pointer, write-through, NULL input
 *   - NULL safety: read/write/load_rom/set_callbacks/get_ram with NULL mem
 */

#include "bbc_memory.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

/* --------------------------------------------------------------------------
 * Minimal test framework
 * -------------------------------------------------------------------------- */
static int s_pass = 0, s_fail = 0;
static const char *s_suite = "";

#define SUITE(name) do { s_suite = (name); printf("\n[%s]\n", name); } while(0)

#define ASSERT_EQ(a, b) do { \
    if ((uint32_t)(a) != (uint32_t)(b)) { \
        printf("  FAIL %s:%d: expected 0x%02X, got 0x%02X\n", \
               s_suite, __LINE__, (unsigned)(b), (unsigned)(a)); \
        s_fail++; \
    } else { s_pass++; } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        printf("  FAIL %s:%d: expected true\n", s_suite, __LINE__); \
        s_fail++; \
    } else { s_pass++; } \
} while(0)

#define ASSERT_FALSE(x) do { \
    if (x) { \
        printf("  FAIL %s:%d: expected false\n", s_suite, __LINE__); \
        s_fail++; \
    } else { s_pass++; } \
} while(0)

#define ASSERT_NOT_NULL(p) do { \
    if ((p) == NULL) { \
        printf("  FAIL %s:%d: expected non-NULL\n", s_suite, __LINE__); \
        s_fail++; \
    } else { s_pass++; } \
} while(0)

#define ASSERT_NULL(p) do { \
    if ((p) != NULL) { \
        printf("  FAIL %s:%d: expected NULL\n", s_suite, __LINE__); \
        s_fail++; \
    } else { s_pass++; } \
} while(0)

/* --------------------------------------------------------------------------
 * Callback helpers
 * -------------------------------------------------------------------------- */

typedef struct {
    uint16_t last_addr;
    uint8_t  last_val;
    int      call_count;
    uint8_t  return_val;
} CbState;

static uint8_t cb_read(uint16_t addr, void *ud) {
    CbState *s = (CbState *)ud;
    s->last_addr = addr;
    s->call_count++;
    return s->return_val;
}

static void cb_write(uint16_t addr, uint8_t val, void *ud) {
    CbState *s = (CbState *)ud;
    s->last_addr = addr;
    s->last_val  = val;
    s->call_count++;
}

/* Two separate userdata pointers to test independent rd/wr userdata */
static CbState s_rd_state;
static CbState s_wr_state;

static uint8_t cb_read_A(uint16_t addr, void *ud) {
    (void)addr;
    CbState *s = (CbState *)ud;
    s->call_count++;
    return s->return_val;
}

static void cb_write_B(uint16_t addr, uint8_t val, void *ud) {
    (void)addr; (void)val;
    CbState *s = (CbState *)ud;
    s->call_count++;
}

/* --------------------------------------------------------------------------
 * 1. Lifecycle
 * -------------------------------------------------------------------------- */
static void test_lifecycle(void)
{
    SUITE("lifecycle");

    BBCMemory *mem = bbc_memory_create();
    ASSERT_NOT_NULL(mem);

    /* destroy should not crash */
    bbc_memory_destroy(mem);

    /* destroy(NULL) is safe — free(NULL) is defined behaviour */
    bbc_memory_destroy(NULL);   /* must not crash */
    s_pass++;   /* reaching here = pass */
}

/* --------------------------------------------------------------------------
 * 2. Initial state
 * -------------------------------------------------------------------------- */
static void test_initial_state(void)
{
    SUITE("initial state");

    BBCMemory *mem = bbc_memory_create();

    /* RAM initialized to 0x00 */
    ASSERT_EQ(bbc_memory_read(mem, 0x0000), 0x00);
    ASSERT_EQ(bbc_memory_read(mem, 0x3FFF), 0x00);
    ASSERT_EQ(bbc_memory_read(mem, 0x7FFF), 0x00);

    /* Sideways ROM unloaded → 0xFF */
    ASSERT_EQ(bbc_memory_read(mem, 0x8000), 0xFF);
    ASSERT_EQ(bbc_memory_read(mem, 0xBFFF), 0xFF);

    /* OS ROM unloaded → 0xFF */
    ASSERT_EQ(bbc_memory_read(mem, 0xC000), 0xFF);
    ASSERT_EQ(bbc_memory_read(mem, 0xFBFF), 0xFF);

    /* I/O region, no callback, no OS ROM → 0xFF */
    ASSERT_EQ(bbc_memory_read(mem, 0xFC00), 0xFF);
    ASSERT_EQ(bbc_memory_read(mem, 0xFE40), 0xFF);
    ASSERT_EQ(bbc_memory_read(mem, 0xFFFF), 0xFF);

    bbc_memory_destroy(mem);
}

/* --------------------------------------------------------------------------
 * 3. RAM read/write
 * -------------------------------------------------------------------------- */
static void test_ram(void)
{
    SUITE("RAM");

    BBCMemory *mem = bbc_memory_create();

    /* Simple round-trip */
    bbc_memory_write(mem, 0x0000, 0x42);
    ASSERT_EQ(bbc_memory_read(mem, 0x0000), 0x42);

    bbc_memory_write(mem, 0x7FFF, 0xBE);
    ASSERT_EQ(bbc_memory_read(mem, 0x7FFF), 0xBE);

    bbc_memory_write(mem, 0x0100, 0x01);
    ASSERT_EQ(bbc_memory_read(mem, 0x0100), 0x01);

    bbc_memory_write(mem, 0x4000, 0xAB);
    ASSERT_EQ(bbc_memory_read(mem, 0x4000), 0xAB);

    /* Writes are independent — no aliasing */
    bbc_memory_write(mem, 0x0001, 0x11);
    bbc_memory_write(mem, 0x0002, 0x22);
    ASSERT_EQ(bbc_memory_read(mem, 0x0001), 0x11);
    ASSERT_EQ(bbc_memory_read(mem, 0x0002), 0x22);

    /* Write at 0x7FFF does not affect 0x8000 (ROM boundary) */
    bbc_memory_write(mem, 0x7FFF, 0xFF);
    ASSERT_EQ(bbc_memory_read(mem, 0x8000), 0xFF);   /* ROM still 0xFF (unloaded) */

    bbc_memory_destroy(mem);
}

/* --------------------------------------------------------------------------
 * 4. Sideways ROM (0x8000–0xBFFF)
 * -------------------------------------------------------------------------- */
static void test_sideways_rom(void)
{
    SUITE("sideways ROM");

    BBCMemory *mem = bbc_memory_create();

    /* Build a recognisable 16 KB ROM image */
    static uint8_t rom16[16384];
    for (int i = 0; i < 16384; i++) rom16[i] = (uint8_t)(i & 0xFF);

    /* Load and verify */
    bbc_memory_load_rom(mem, rom16, 16384, 0x8000);
    ASSERT_EQ(bbc_memory_read(mem, 0x8000), rom16[0]);
    ASSERT_EQ(bbc_memory_read(mem, 0x8001), rom16[1]);
    ASSERT_EQ(bbc_memory_read(mem, 0x9000), rom16[0x1000]);
    ASSERT_EQ(bbc_memory_read(mem, 0xBFFE), rom16[0x3FFE]);
    ASSERT_EQ(bbc_memory_read(mem, 0xBFFF), rom16[0x3FFF]);

    /* Partial load: 256 bytes only; rest of ROM stays 0xFF */
    static uint8_t rom_small[256];
    for (int i = 0; i < 256; i++) rom_small[i] = 0xA0 + (uint8_t)i;
    BBCMemory *mem2 = bbc_memory_create();
    bbc_memory_load_rom(mem2, rom_small, 256, 0x8000);
    ASSERT_EQ(bbc_memory_read(mem2, 0x8000), 0xA0);
    ASSERT_EQ(bbc_memory_read(mem2, 0x80FF), rom_small[255]);
    ASSERT_EQ(bbc_memory_read(mem2, 0x8100), 0xFF);   /* unpopulated tail */
    ASSERT_EQ(bbc_memory_read(mem2, 0xBFFF), 0xFF);
    bbc_memory_destroy(mem2);

    /* Oversized load (> 16384) is silently ignored */
    BBCMemory *mem3 = bbc_memory_create();
    static uint8_t big[16385];
    memset(big, 0x55, sizeof(big));
    bbc_memory_load_rom(mem3, big, 16385, 0x8000);
    ASSERT_EQ(bbc_memory_read(mem3, 0x8000), 0xFF);   /* not loaded */
    bbc_memory_destroy(mem3);

    /* Wrong startAddr (not 0x8000 or 0xC000) is silently ignored */
    BBCMemory *mem4 = bbc_memory_create();
    static uint8_t rom_bad[256];
    memset(rom_bad, 0x77, sizeof(rom_bad));
    bbc_memory_load_rom(mem4, rom_bad, 256, 0x9000);
    ASSERT_EQ(bbc_memory_read(mem4, 0x9000), 0xFF);   /* not loaded */
    bbc_memory_destroy(mem4);

    bbc_memory_destroy(mem);
}

/* --------------------------------------------------------------------------
 * 5. OS ROM (0xC000–0xFBFF)
 * -------------------------------------------------------------------------- */
static void test_os_rom(void)
{
    SUITE("OS ROM");

    BBCMemory *mem = bbc_memory_create();

    /* Build a recognisable 16 KB OS image */
    static uint8_t os16[16384];
    for (int i = 0; i < 16384; i++) os16[i] = (uint8_t)(0x80 ^ (i & 0xFF));

    bbc_memory_load_rom(mem, os16, 16384, 0xC000);

    /* Reads from OS ROM region (0xC000–0xFBFF) */
    ASSERT_EQ(bbc_memory_read(mem, 0xC000), os16[0x0000]);
    ASSERT_EQ(bbc_memory_read(mem, 0xC001), os16[0x0001]);
    ASSERT_EQ(bbc_memory_read(mem, 0xD000), os16[0x1000]);
    ASSERT_EQ(bbc_memory_read(mem, 0xFBFF), os16[0xFBFF - 0xC000]);

    /* I/O region (0xFC00–0xFFFF) falls through to OS ROM when no callback */
    ASSERT_EQ(bbc_memory_read(mem, 0xFC00), os16[0xFC00 - 0xC000]);
    ASSERT_EQ(bbc_memory_read(mem, 0xFFFF), os16[0xFFFF - 0xC000]);
    ASSERT_EQ(bbc_memory_read(mem, 0xFFFD), os16[0xFFFD - 0xC000]);  /* reset vector lo */

    bbc_memory_destroy(mem);
}

/* --------------------------------------------------------------------------
 * 6. ROM write protection — writes silently dropped
 * -------------------------------------------------------------------------- */
static void test_rom_write_protection(void)
{
    SUITE("ROM write protection");

    BBCMemory *mem = bbc_memory_create();

    static uint8_t rom[16384];
    memset(rom, 0xCC, sizeof(rom));
    bbc_memory_load_rom(mem, rom, 16384, 0x8000);
    bbc_memory_load_rom(mem, rom, 16384, 0xC000);

    /* Write to sideways ROM — silently dropped */
    bbc_memory_write(mem, 0x8000, 0x00);
    ASSERT_EQ(bbc_memory_read(mem, 0x8000), 0xCC);

    bbc_memory_write(mem, 0xBFFF, 0x00);
    ASSERT_EQ(bbc_memory_read(mem, 0xBFFF), 0xCC);

    /* Write to OS ROM (below IO_START) — silently dropped */
    bbc_memory_write(mem, 0xC000, 0x00);
    ASSERT_EQ(bbc_memory_read(mem, 0xC000), 0xCC);

    bbc_memory_write(mem, 0xFBFF, 0x00);
    ASSERT_EQ(bbc_memory_read(mem, 0xFBFF), 0xCC);

    bbc_memory_destroy(mem);
}

/* --------------------------------------------------------------------------
 * 7. I/O read callbacks
 * -------------------------------------------------------------------------- */
static void test_read_callbacks(void)
{
    SUITE("read callbacks");

    BBCMemory *mem = bbc_memory_create();
    CbState st = {0};

    /* Register at a typical address — CRTC at 0xFE00 */
    st.return_val = 0x5A;
    bbc_memory_set_read_callback(mem, 0xFE00, cb_read, &st);
    uint8_t v = bbc_memory_read(mem, 0xFE00);
    ASSERT_EQ(v, 0x5A);
    ASSERT_EQ(st.last_addr, 0xFE00);
    ASSERT_EQ(st.call_count, 1);

    /* First slot (0xFC00) */
    CbState st2 = {0};
    st2.return_val = 0x11;
    bbc_memory_set_read_callback(mem, 0xFC00, cb_read, &st2);
    ASSERT_EQ(bbc_memory_read(mem, 0xFC00), 0x11);
    ASSERT_EQ(st2.last_addr, 0xFC00);
    ASSERT_EQ(st2.call_count, 1);

    /* Last slot (0xFFFF) */
    CbState st3 = {0};
    st3.return_val = 0x99;
    bbc_memory_set_read_callback(mem, 0xFFFF, cb_read, &st3);
    ASSERT_EQ(bbc_memory_read(mem, 0xFFFF), 0x99);
    ASSERT_EQ(st3.last_addr, 0xFFFF);

    /* Callback at one address does not affect adjacent address */
    ASSERT_EQ(st.call_count, 1);   /* 0xFE00 callback called only once */
    bbc_memory_read(mem, 0xFE01);  /* no callback → falls through */
    ASSERT_EQ(st.call_count, 1);   /* still 1 */

    /* Address below IO_START (0xFBFF) → silently ignored, no registration */
    CbState st4 = {0};
    st4.return_val = 0xAB;
    bbc_memory_set_read_callback(mem, 0xFBFF, cb_read, &st4);
    /* Reading 0xFBFF should NOT call this callback (it's in OS ROM region) */
    bbc_memory_read(mem, 0xFBFF);
    ASSERT_EQ(st4.call_count, 0);

    /* Clearing a callback by passing NULL → falls through to ROM */
    bbc_memory_set_read_callback(mem, 0xFE00, NULL, NULL);
    bbc_memory_read(mem, 0xFE00);
    ASSERT_EQ(st.call_count, 1);   /* still 1 — callback was cleared */

    bbc_memory_destroy(mem);
}

/* --------------------------------------------------------------------------
 * 8. I/O write callbacks
 * -------------------------------------------------------------------------- */
static void test_write_callbacks(void)
{
    SUITE("write callbacks");

    BBCMemory *mem = bbc_memory_create();
    CbState st = {0};

    /* Register at Video ULA address */
    bbc_memory_set_write_callback(mem, 0xFE20, cb_write, &st);
    bbc_memory_write(mem, 0xFE20, 0x07);
    ASSERT_EQ(st.last_addr, 0xFE20);
    ASSERT_EQ(st.last_val,  0x07);
    ASSERT_EQ(st.call_count, 1);

    /* First slot */
    CbState st2 = {0};
    bbc_memory_set_write_callback(mem, 0xFC00, cb_write, &st2);
    bbc_memory_write(mem, 0xFC00, 0xBB);
    ASSERT_EQ(st2.last_val, 0xBB);
    ASSERT_EQ(st2.call_count, 1);

    /* Last slot */
    CbState st3 = {0};
    bbc_memory_set_write_callback(mem, 0xFFFF, cb_write, &st3);
    bbc_memory_write(mem, 0xFFFF, 0xCC);
    ASSERT_EQ(st3.last_val, 0xCC);

    /* Write to address with no callback → silently dropped, no crash */
    bbc_memory_write(mem, 0xFE01, 0x55);   /* no callback registered */
    s_pass++;   /* reaching here = pass */

    /* Address below IO_START silently ignored */
    CbState st4 = {0};
    bbc_memory_set_write_callback(mem, 0xFBFF, cb_write, &st4);
    bbc_memory_write(mem, 0xFBFF, 0xFF);   /* this writes to... nothing (ROM) */
    ASSERT_EQ(st4.call_count, 0);   /* callback never registered */

    bbc_memory_destroy(mem);
}

/* --------------------------------------------------------------------------
 * 9. Independent read/write userdata at same address
 * -------------------------------------------------------------------------- */
static void test_independent_userdata(void)
{
    SUITE("independent userdata");

    BBCMemory *mem = bbc_memory_create();

    memset(&s_rd_state, 0, sizeof(s_rd_state));
    memset(&s_wr_state, 0, sizeof(s_wr_state));
    s_rd_state.return_val = 0xDD;

    /* Register read and write callbacks with different userdata */
    bbc_memory_set_read_callback (mem, 0xFE40, cb_read_A, &s_rd_state);
    bbc_memory_set_write_callback(mem, 0xFE40, cb_write_B, &s_wr_state);

    /* Read fires with s_rd_state */
    bbc_memory_read(mem, 0xFE40);
    ASSERT_EQ(s_rd_state.call_count, 1);
    ASSERT_EQ(s_wr_state.call_count, 0);

    /* Write fires with s_wr_state */
    bbc_memory_write(mem, 0xFE40, 0x12);
    ASSERT_EQ(s_rd_state.call_count, 1);   /* unchanged */
    ASSERT_EQ(s_wr_state.call_count, 1);

    bbc_memory_destroy(mem);
}

/* --------------------------------------------------------------------------
 * 10. Range callbacks
 * -------------------------------------------------------------------------- */
static void test_range_callbacks(void)
{
    SUITE("range callbacks");

    BBCMemory *mem = bbc_memory_create();
    CbState st = {0};
    st.return_val = 0x7E;

    /* Register VIA range: 0xFE40–0xFE4F (16 bytes) */
    bbc_memory_set_range_callbacks(mem, 0xFE40, 16, cb_read, cb_write, &st);

    /* All addresses in range trigger callback */
    for (int a = 0xFE40; a <= 0xFE4F; a++) {
        st.call_count = 0;
        uint8_t v = bbc_memory_read(mem, (uint16_t)a);
        ASSERT_EQ(v, 0x7E);
        ASSERT_EQ(st.call_count, 1);
    }

    /* Address just outside range (0xFE50) is NOT registered */
    st.call_count = 0;
    bbc_memory_read(mem, 0xFE50);
    ASSERT_EQ(st.call_count, 0);

    /* Write side of range */
    CbState wst = {0};
    bbc_memory_set_range_callbacks(mem, 0xFE60, 16, NULL, cb_write, &wst);
    bbc_memory_write(mem, 0xFE60, 0x55);
    ASSERT_EQ(wst.last_addr, 0xFE60);
    ASSERT_EQ(wst.last_val,  0x55);
    ASSERT_EQ(wst.call_count, 1);
    bbc_memory_write(mem, 0xFE6F, 0xAA);
    ASSERT_EQ(wst.last_addr, 0xFE6F);
    ASSERT_EQ(wst.last_val,  0xAA);

    /* Range below IO_START silently rejected */
    CbState st2 = {0};
    bbc_memory_set_range_callbacks(mem, 0xFBFF, 4, cb_read, cb_write, &st2);
    bbc_memory_read(mem, 0xFBFF);
    ASSERT_EQ(st2.call_count, 0);

    bbc_memory_destroy(mem);
}

/* --------------------------------------------------------------------------
 * 11. bbc_memory_get_ram
 * -------------------------------------------------------------------------- */
static void test_get_ram(void)
{
    SUITE("get_ram");

    BBCMemory *mem = bbc_memory_create();

    uint8_t *ram = bbc_memory_get_ram(mem);
    ASSERT_NOT_NULL(ram);

    /* Write via pointer, read back via bbc_memory_read */
    ram[0x1234] = 0xCA;
    ASSERT_EQ(bbc_memory_read(mem, 0x1234), 0xCA);

    ram[0x0000] = 0xFE;
    ASSERT_EQ(bbc_memory_read(mem, 0x0000), 0xFE);

    ram[0x7FFF] = 0xED;
    ASSERT_EQ(bbc_memory_read(mem, 0x7FFF), 0xED);

    /* Write via bbc_memory_write, read back via pointer */
    bbc_memory_write(mem, 0x0200, 0x42);
    ASSERT_EQ(ram[0x0200], 0x42);

    /* NULL input returns NULL */
    ASSERT_NULL(bbc_memory_get_ram(NULL));

    bbc_memory_destroy(mem);
}

/* --------------------------------------------------------------------------
 * 12. NULL safety
 * -------------------------------------------------------------------------- */
static void test_null_safety(void)
{
    SUITE("NULL safety");

    /* bbc_memory_read(NULL) → 0xFF, no crash */
    uint8_t v = bbc_memory_read(NULL, 0x0000);
    ASSERT_EQ(v, 0xFF);

    /* bbc_memory_write(NULL) → no crash */
    bbc_memory_write(NULL, 0x0000, 0x42);
    s_pass++;

    /* bbc_memory_load_rom(NULL) → no crash */
    static uint8_t dummy[16] = {0};
    bbc_memory_load_rom(NULL, dummy, 16, 0x8000);
    s_pass++;

    /* bbc_memory_load_rom with NULL romData → no crash */
    BBCMemory *mem = bbc_memory_create();
    bbc_memory_load_rom(mem, NULL, 16, 0x8000);
    s_pass++;
    ASSERT_EQ(bbc_memory_read(mem, 0x8000), 0xFF);  /* not loaded */

    /* set_read_callback(NULL) → no crash */
    bbc_memory_set_read_callback(NULL, 0xFE40, cb_read, NULL);
    s_pass++;

    /* set_write_callback(NULL) → no crash */
    bbc_memory_set_write_callback(NULL, 0xFE40, cb_write, NULL);
    s_pass++;

    /* set_range_callbacks(NULL) → no crash */
    bbc_memory_set_range_callbacks(NULL, 0xFE40, 16, cb_read, cb_write, NULL);
    s_pass++;

    /* get_ram(NULL) → NULL */
    ASSERT_NULL(bbc_memory_get_ram(NULL));

    bbc_memory_destroy(mem);
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */
int main(void)
{
    printf("bbc_memory tests\n");
    printf("================\n");

    test_lifecycle();
    test_initial_state();
    test_ram();
    test_sideways_rom();
    test_os_rom();
    test_rom_write_protection();
    test_read_callbacks();
    test_write_callbacks();
    test_independent_userdata();
    test_range_callbacks();
    test_get_ram();
    test_null_safety();

    printf("\nResults: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
