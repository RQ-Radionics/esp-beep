/*
 * diag_kbd.c — BBC Micro keyboard diagnosis tool
 *
 * Runs the machine headlessly, injects a key, and checks if the MOS
 * puts a character in the keyboard input buffer.
 *
 * BBC OS key buffer: circular buffer at 0x02xx.
 *   &02CF = buffer end pointer (default &02FF)
 *   &02D0 = get pointer (tail)
 *   &02D1 = put pointer (head)
 *   Characters are at 0x0200 + ptr
 *
 * Build + run:  make diag && ./diag_kbd
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "bbc_machine.h"
#include "bbc_memory.h"
#include "bbc_video.h"

static uint8_t *load_rom(const char *path, uint32_t *sz)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    uint8_t *b = malloc((size_t)n);
    if (!b || (long)fread(b, 1, (size_t)n, f) != n) { free(b); fclose(f); return NULL; }
    fclose(f); *sz = (uint32_t)n; return b;
}

static int g_irq_count = 0;
static void (*g_orig_irq)(void *ctx, bool state);
static void diag_irq(void *ctx, bool state)
{
    if (state) g_irq_count++;
    if (g_orig_irq) g_orig_irq(ctx, state);
}

int main(void)
{
    uint32_t os_size = 0, basic_size = 0;
    uint8_t *os    = load_rom("../../rom/os12.rom",   &os_size);
    uint8_t *basic = load_rom("../../rom/basic2.rom", &basic_size);
    if (!os || !basic) return 1;

    bbc_machine_t *m = calloc(1, sizeof(*m));
    if (!m) return 1;

    bbc_machine_init(m, os, os_size, basic, basic_size);
    g_orig_irq = m->sysvia.cb.irq;
    m->sysvia.cb.irq = diag_irq;
    bbc_machine_reset(m);

    uint8_t *ram = bbc_memory_get_ram(m->mem);

    printf("[diag] Booting 2M cycles...\n");
    long total = 0;
    while (total < 2000000L)
        total += bbc_machine_step(m);

    printf("[diag] After 2M cycles:\n");
    printf("  OS keyboard buf: get=$02D8=%02X put=$02E1=%02X  (legacy: $02D0=%02X $02D1=%02X)\n",
           ram[0x02D8], ram[0x02E1], ram[0x02D0], ram[0x02D1]);
    printf("  INSV vector $0220/$0221 = %02X %02X → $%02X%02X\n",
           ram[0x0220], ram[0x0221], ram[0x0221], ram[0x0220]);
    printf("  KEYV $022A/$022B = %02X %02X → $%02X%02X\n",
           ram[0x022A], ram[0x022B], ram[0x022B], ram[0x022A]);
    printf("  IER=%02X PCR=%02X CA2_in=%d\n",
           m->sysvia.via.ier, m->sysvia.via.pcr,
           m->sysvia.via.pa.c2_in);

    /* Inject A (row=4, col=1) */
    printf("\n[diag] Pressing A (row=4, col=1)...\n");
    bbc_machine_key_event(m, 4, 1, true);

    uint8_t put_before = ram[0x02E1];  /* $02E1 = put-ptr for buffer 0 (keyboard) */
    printf("  Buffer put ptr ($02E1) before: %02X\n", put_before);

    /* Run up to 6M cycles (3 sec BBC time), report as soon as buffer changes.
     * Also track $E7 (key repeat counter) to see if it ever decrements. */
    long end = total + 6000000L;
    uint8_t put_after = put_before;
    long detected_at = -1;
    uint8_t e7_prev = ram[0xE7];
    int e7_trace_count = 0;
    int pc_trace_count = 0;
    while (total < end) {
        uint16_t pc_before = bbc_cpu_get_pc(m->cpu);
        total += bbc_machine_step(m);
        uint8_t e7_now = ram[0xE7];
        /* Trace E7 changes around 0, and also capture EC/buffer changes */
        if (e7_now != e7_prev) {
            if ((e7_now <= 3 || e7_prev <= 3) && e7_trace_count < 40) {
                fprintf(stderr, "[E7] PC=%04X: $E7 %02X->%02X EC=%02X buf_put=%02X FA=%02X 025A=%02X 0259=%02X 026C=%02X\n",
                        pc_before, e7_prev, e7_now,
                        ram[0xEC], ram[0x02D1], ram[0xFA], ram[0x025A],
                        ram[0x0259], ram[0x026C]);
                e7_trace_count++;
            }
            e7_prev = e7_now;
        }
        /* Trace key processing path: EF91 (translate), EFD1 (compare), EFE1 (0259 check),
         * EFE6 (buffer insert), E4A8 (KEYV), E4B3 (buffer write) */
        if (((pc_before >= 0xEF91 && pc_before <= 0xEFE9) ||
             (pc_before >= 0xE4A8 && pc_before <= 0xE4D3)) && pc_trace_count < 120) {
            fprintf(stderr, "[PC] %04X  EC=%02X FA=%02X 025A=%02X 0259=%02X 026C=%02X put=$02E1=%02X get=$02D8=%02X\n",
                    pc_before, ram[0xEC], ram[0xFA], ram[0x025A],
                    ram[0x0259], ram[0x026C], ram[0x02E1], ram[0x02D8]);
            pc_trace_count++;
        }
        if (ram[0x02E1] != put_before && detected_at < 0) {
            detected_at = total;
            put_after = ram[0x02E1];
            break;
        }
    }
    if (detected_at < 0) put_after = ram[0x02E1];
    printf("  Buffer put ptr ($02E1) after:  %02X  (cycles: %ld)\n",
           put_after, detected_at >= 0 ? detected_at : end);

    if (put_after != put_before) {
        printf("  [SUCCESS] MOS detected keypress!\n");
        /* Dump buffer — keyboard buffer 0 uses circular buffer with:
         * get-ptr at $02D8, put-ptr at $02E1, wraps at $02E8 end */
        uint8_t get_ptr = ram[0x02D8];
        uint8_t put_ptr = ram[0x02E1];
        printf("  Key buffer: get=$02D8=%02X put=$02E1=%02X\n", get_ptr, put_ptr);
        printf("  Buffer area $02E8-$02FF (and $02E8+ptr):\n  ");
        for (int i = 0x02E8; i <= 0x02FF; i++)
            printf("%02X ", ram[i]);
        printf("\n");
    } else {
        printf("  [FAIL] No character in buffer after run\n");
        printf("  VIA: IER=%02X IFR=%02X IC32=%02X KBD_WE=%d\n",
               m->sysvia.via.ier, m->sysvia.via.ifr,
               m->sysvia.latch, (m->sysvia.latch>>3)&1);
        printf("  PA outr=%02X (row=%d col=%d)\n",
               m->sysvia.via.pa.outr,
               (m->sysvia.via.pa.outr>>4)&7,
               m->sysvia.via.pa.outr & 0xF);
        printf("  CA2_in=%d irq_out=%d\n",
               m->sysvia.via.pa.c2_in, m->sysvia.via.irq_out);
        printf("  IRQs fired: %d\n", g_irq_count);

        /* Dump buf descriptor area */
        printf("  RAM[$02D8-$02EA]: ");
        for (int i = 0x02D8; i <= 0x02EA; i++)
            printf("%02X ", ram[i]);
        printf("\n");
        printf("  ($02D8=get_ptr, $02E1=put_ptr)\n");
        /* BBC OS key state ZP vars */
        printf("  ZP[E6..F2]:       ");
        for (int i = 0xE6; i <= 0xF2; i++)
            printf("%02X ", ram[i]);
        printf("\n");
        printf("    (E7=repeat_ctr EC=key0 ED=key1 EF=kbd_state)\n");
        printf("  ZP[C6]:           %02X (INKEY timeout)\n", ram[0xC6]);
        printf("  RAM[0247]:        %02X (key state flags)\n", ram[0x0247]);
        printf("  RAM[025A]:        %02X (keyboard state)\n", ram[0x025A]);
        printf("  RAM[0242]:        %02X (key repeat)\n", ram[0x0242]);
        printf("  RAM[0254]:        %02X (key repeat initial delay)\n", ram[0x0254]);
        printf("  INSV $0220=%02X%02X KEYV $022A=%02X%02X\n",
               ram[0x0221], ram[0x0220], ram[0x022B], ram[0x022A]);
        printf("  $E447,X (buf0 wrap): %02X\n", ram[0xE447]);
    }

    /* Also try RETURN key (row=4, col=9) */
    printf("\n[diag] Adding RETURN (row=4, col=9)...\n");
    bbc_machine_key_event(m, 4, 1, false);
    bbc_machine_key_event(m, 4, 9, true);

    put_before = ram[0x02E1];
    end = total + 200000L;
    while (total < end)
        total += bbc_machine_step(m);

    put_after = ram[0x02E1];
    bbc_machine_key_event(m, 4, 9, false);

    if (put_after != put_before)
        printf("  [SUCCESS] RETURN detected!\n");
    else
        printf("  [FAIL] RETURN not detected either\n");

    free(m); free(os); free(basic);
    return 0;
}
