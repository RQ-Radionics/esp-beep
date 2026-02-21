/*
 * Klaus2m5 6502 Interrupt Test harness
 *
 * Loads 6502_interrupt_test.bin into a 64KB RAM image and runs it.
 * Implements the feedback register at $BFFC used by the test to trigger
 * IRQ and NMI lines:
 *   bit 0 written -> assert IRQ  (open-collector, I_drive=1: bit SET = asserted)
 *   bit 1 written -> assert NMI
 *   bit 7 = diag-stop, filtered out (I_filter=$7F)
 *
 * Trap detection uses GetPC() (the address about to execute) rather than
 * GetCurrentOpcodeAddr() which is stale during interrupt service cycles.
 *
 * PASS: trap at $4C (JMP *) = success macro
 * FAIL: any other opcode at the trap PC
 */

#include "vrEmu6502.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#define ENTRY_ADDR   0x0400
#define I_PORT       0xBFFC   /* feedback register address */
#define IRQ_BIT      0        /* bit 0 = IRQ */
#define NMI_BIT      1        /* bit 1 = NMI */
#define I_FILTER     0x7F     /* bit 7 = diag stop, filtered out */
#define BIN_SIZE     0x10000

static uint8_t ram[BIN_SIZE];
static VrEmu6502 *cpu;

/* Current state of the feedback register */
static uint8_t i_port_val = 0;

static uint8_t MemRead(uint16_t addr, bool isDbg)
{
    (void)isDbg;
    if (addr == I_PORT)
        return i_port_val;
    return ram[addr];
}

static void MemWrite(uint16_t addr, uint8_t val)
{
    if (addr == I_PORT) {
        uint8_t prev = i_port_val;
        i_port_val = val & I_FILTER;  /* mask diag-stop bit */

        /* open collector, I_drive=1: rising edge on bit = interrupt ASSERTED */
        if ((i_port_val ^ prev) & (1 << IRQ_BIT)) {
            *vrEmu6502Int(cpu) = (i_port_val & (1 << IRQ_BIT))
                                 ? IntRequested : IntCleared;
        }
        if ((i_port_val ^ prev) & (1 << NMI_BIT)) {
            *vrEmu6502Nmi(cpu) = (i_port_val & (1 << NMI_BIT))
                                 ? IntRequested : IntCleared;
        }
        return;
    }
    ram[addr] = val;
}

int main(int argc, char *argv[])
{
    const char *binpath = (argc > 1) ? argv[1] : "6502_interrupt_test.bin";

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

    /* Zero out ZP and data areas (as a real reset would leave them) */
    memset(ram + 0x000A, 0, 6);    /* zero_page: irq_a..nmi_f */
    memset(ram + 0x0200, 0, 4);    /* data_segment: nmi_count..I_src */

    printf("Klaus2m5 6502 Interrupt Test\n");
    printf("  Binary:        %s (%zu bytes)\n", binpath, n);
    printf("  Entry:         $%04X\n", ENTRY_ADDR);
    printf("  Feedback port: $%04X  (IRQ=bit%d, NMI=bit%d)\n",
           I_PORT, IRQ_BIT, NMI_BIT);
    printf("  NMI=$%04X  RESET=$%04X  IRQ=$%04X\n",
           (uint16_t)(ram[0xFFFA] | (ram[0xFFFB] << 8)),
           (uint16_t)(ram[0xFFFC] | (ram[0xFFFD] << 8)),
           (uint16_t)(ram[0xFFFE] | (ram[0xFFFF] << 8)));
    printf("Running...\n\n");

    cpu = vrEmu6502New(CPU_6502, MemRead, MemWrite);
    if (!cpu) {
        fprintf(stderr, "ERROR: vrEmu6502New failed\n");
        return 1;
    }

    vrEmu6502Reset(cpu);
    vrEmu6502SetPC(cpu, ENTRY_ADDR);

    /* Trap detection: sample PC before each instruction (GetPC() is correct
       even during interrupt service, unlike GetCurrentOpcodeAddr() which is
       stale until the first opcode fetch of the handler completes). */
    uint16_t lastPc = ENTRY_ADDR - 1;
    uint64_t instructions = 0;
    uint64_t cycles = 0;
    clock_t t0 = clock();

    while (1) {
        uint16_t pc = vrEmu6502GetPC(cpu);
        if (vrEmu6502GetOpcodeCycle(cpu) == 0) {
            if (pc == lastPc) {
                uint8_t opcode = ram[pc];
                clock_t t1 = clock();
                double secs = (double)(t1 - t0) / CLOCKS_PER_SEC;

                printf("Trap at PC=$%04X  opcode=$%02X\n", pc, opcode);
                printf("  I_src=$%02X  nmi_count=$%02X  irq_count=$%02X  brk_count=$%02X\n",
                       ram[0x0203], ram[0x0200], ram[0x0201], ram[0x0202]);
                printf("Instructions: %llu  Cycles: %llu\n",
                       (unsigned long long)instructions,
                       (unsigned long long)cycles);
                printf("Elapsed: %.3f s\n\n", secs);

                if (opcode == 0x4C) {
                    printf("RESULT: PASSED\n");
                    vrEmu6502Destroy(cpu);
                    return 0;
                } else {
                    printf("RESULT: FAILED (trapped at non-JMP opcode)\n");
                    vrEmu6502Destroy(cpu);
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
