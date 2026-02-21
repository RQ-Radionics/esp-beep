/*
 * Bruce Clark / Klaus2m5 6502 Decimal Mode Test harness
 *
 * Tests BCD arithmetic (ADC/SBC in decimal mode) for all N1/N2 combinations.
 * Source: https://github.com/Klaus2m5/6502_65C02_functional_tests
 * ca65 port: https://github.com/amb5l/6502_65C02_functional_tests
 *
 * Layout:
 *   Zero page $00-$0F: test variables (N1, N2, ERROR at $0B, ...)
 *   Code at $0400, entry at $0400
 *
 * Termination: JMP * at DONE label
 *   ERROR ($0B) == 0 -> PASS
 *   ERROR ($0B) == 1 -> FAIL
 */

#include "vrEmu6502.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#define ENTRY_ADDR   0x0400
#define ERROR_ADDR   0x000B   /* zero-page byte: 0=pass, 1=fail */
#define BIN_SIZE     0x10000

static uint8_t ram[BIN_SIZE];

static uint8_t MemRead(uint16_t addr, bool isDbg)
{
    (void)isDbg;
    return ram[addr];
}

static void MemWrite(uint16_t addr, uint8_t val)
{
    ram[addr] = val;
}

int main(int argc, char *argv[])
{
    const char *binpath = (argc > 1) ? argv[1] : "6502_decimal_test.bin";

    FILE *f = fopen(binpath, "rb");
    if (!f) {
        fprintf(stderr, "ERROR: cannot open %s\n", binpath);
        return 1;
    }
    memset(ram, 0xff, sizeof(ram));
    size_t n = fread(ram, 1, BIN_SIZE, f);
    fclose(f);
    if (n != BIN_SIZE) {
        fprintf(stderr, "ERROR: expected %d bytes, got %zu\n", BIN_SIZE, n);
        return 1;
    }

    printf("Bruce Clark 6502 Decimal Mode Test\n");
    printf("  Binary: %s (%zu bytes)\n", binpath, n);
    printf("  Entry:  $%04X\n", ENTRY_ADDR);
    printf("  Config: cputype=6502 vld_bcd=0 chk_a/n/v/z/c=1\n");
    printf("Running...\n\n");

    VrEmu6502 *cpu = vrEmu6502New(CPU_6502, MemRead, MemWrite);
    if (!cpu) {
        fprintf(stderr, "ERROR: vrEmu6502New failed\n");
        return 1;
    }

    vrEmu6502Reset(cpu);
    vrEmu6502SetPC(cpu, ENTRY_ADDR);

    uint16_t lastPc = ENTRY_ADDR - 1;
    uint64_t instructions = 0;
    uint64_t cycles = 0;
    clock_t t0 = clock();

    while (1) {
        uint16_t pc = vrEmu6502GetPC(cpu);
        if (vrEmu6502GetOpcodeCycle(cpu) == 0) {
            if (pc == lastPc) {
                clock_t t1 = clock();
                double secs = (double)(t1 - t0) / CLOCKS_PER_SEC;
                uint8_t error = ram[ERROR_ADDR];

                printf("Trap at PC=$%04X  opcode=$%02X\n", pc, ram[pc]);
                printf("  ERROR=$%02X  N1=$%02X  N2=$%02X\n",
                       error, ram[0x00], ram[0x01]);
                printf("Instructions: %llu  Cycles: %llu\n",
                       (unsigned long long)instructions,
                       (unsigned long long)cycles);
                printf("Elapsed: %.3f s\n\n", secs);

                vrEmu6502Destroy(cpu);
                if (ram[pc] == 0x4C && error == 0) {
                    printf("RESULT: PASSED\n");
                    return 0;
                } else {
                    printf("RESULT: FAILED (ERROR=%d at N1=$%02X N2=$%02X)\n",
                           error, ram[0x00], ram[0x01]);
                    return 1;
                }
            }
            lastPc = pc;
            ++instructions;
        }
        cycles += vrEmu6502InstCycle(cpu);
    }

    vrEmu6502Destroy(cpu);
    return 1;
}
