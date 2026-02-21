/*
 * Klaus2m5 6502 Functional Test harness
 *
 * Loads the flat binary (6502_functional_test.bin, 64KB image) into RAM,
 * sets PC = $0400, runs until PC traps (JMP *), and reports PASS/FAIL.
 *
 * PASS: trapped opcode == $4C (JMP), meaning all tests completed OK.
 * FAIL: any other opcode at the trap PC.
 */

#include "vrEmu6502.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#define ENTRY_ADDR  0x0400
#define TEST_CASE_ADDR 0x0200   /* Klaus stores current test number here */
#define BIN_SIZE    0x10000

/* flat 64KB RAM */
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
    const char *binpath = (argc > 1) ? argv[1] : "6502_functional_test.bin";

    /* Load binary */
    FILE *f = fopen(binpath, "rb");
    if (!f) {
        fprintf(stderr, "ERROR: cannot open %s\n", binpath);
        return 1;
    }
    memset(ram, 0, sizeof(ram));
    size_t n = fread(ram, 1, BIN_SIZE, f);
    fclose(f);
    if (n != BIN_SIZE) {
        fprintf(stderr, "ERROR: expected %d bytes, got %zu\n", BIN_SIZE, n);
        return 1;
    }

    printf("Klaus2m5 6502 Functional Test\n");
    printf("  Binary: %s (%zu bytes)\n", binpath, n);
    printf("  Entry:  $%04X\n", ENTRY_ADDR);
    printf("  Reset vector in image: $%04X\n",
           (uint16_t)(ram[0xFFFC] | (ram[0xFFFD] << 8)));
    printf("Running...\n\n");

    VrEmu6502 *cpu = vrEmu6502New(CPU_6502, MemRead, MemWrite);
    if (!cpu) {
        fprintf(stderr, "ERROR: vrEmu6502New failed\n");
        return 1;
    }

    vrEmu6502Reset(cpu);
    vrEmu6502SetPC(cpu, ENTRY_ADDR);

    uint16_t lastPc = ENTRY_ADDR - 1;  /* sentinel: different from entry */
    uint64_t instructions = 0;
    uint64_t cycles = 0;

    clock_t t0 = clock();

    while (1) {
        if (vrEmu6502GetOpcodeCycle(cpu) == 0) {
            uint16_t pc = vrEmu6502GetCurrentOpcodeAddr(cpu);
            if (pc == lastPc) {
                /* Trapped */
                uint8_t opcode = vrEmu6502GetCurrentOpcode(cpu);
                clock_t t1 = clock();
                double secs = (double)(t1 - t0) / CLOCKS_PER_SEC;

                printf("Trap at PC=$%04X  opcode=$%02X  test_case=$%02X\n",
                       pc, opcode, ram[TEST_CASE_ADDR]);
                printf("Instructions: %llu  Cycles: %llu\n",
                       (unsigned long long)instructions,
                       (unsigned long long)cycles);
                printf("Elapsed: %.3f s\n\n", secs);

                if (opcode == 0x4C) {
                    printf("RESULT: PASSED\n");
                    vrEmu6502Destroy(cpu);
                    return 0;
                } else {
                    printf("RESULT: FAILED (stuck at non-JMP opcode)\n");
                    vrEmu6502Destroy(cpu);
                    return 1;
                }
            }
            lastPc = pc;
            ++instructions;
        }
        cycles += vrEmu6502InstCycle(cpu);
    }

    /* unreachable */
    vrEmu6502Destroy(cpu);
    return 1;
}
