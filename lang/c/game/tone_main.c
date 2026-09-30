/* Loads the audio core's bring-up program (lang/c/audio/main.c), starts it
 * and reports what it found: its self-tests (multiply, DMEM sub-word access,
 * SDRAM through its bus master, the APF audio FIFO, its cycle counter, the
 * DSP instructions) and its benchmarks. It then plays a 440 Hz tone for as
 * long as this program runs.
 *
 * Build the audio program first:  make -C ../audio        (-> build/main/)
 * then this one:                  make PROG=tone
 */
#include <stdint.h>
#include <stdio.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <system.h>

#include "audio_load.h"
#ifndef AUDIO_FW_HEADER
#define AUDIO_FW_HEADER "../audio/build/main/audio_fw.h"   /* CPU=mips: build/mips/main (Makefile) */
#endif
#include AUDIO_FW_HEADER

#define ST_TONE 0xA0D10002u          /* lang/c/audio/main.c: tests done, tone playing */

static uint64_t now(void) { timer0_uptime_latch_write(1); return timer0_uptime_cycles_read(); }

static uint32_t s_scratch[128] __attribute__((aligned(64)));   /* SDRAM the audio core tests with */

int main(void)
{
    printf("Mirlo: audio core bring-up (IMEM %u words, DMEM %u words)\n",
           AUDIO_IMEM_NWORDS, AUDIO_DMEM_NWORDS);

    int bad = audio_load(audio_imem_words, AUDIO_IMEM_NWORDS, audio_dmem_words, AUDIO_DMEM_NWORDS);
    printf("  load: %d words read back wrong\n", bad);
    for (int i = 0; i < 128; i++) s_scratch[i] = 0;
    flush_cpu_dcache();
    audio_set_mbox(1, (uint32_t)(uintptr_t)s_scratch);
    audio_start();

    uint64_t end = now() + (uint64_t)CONFIG_CLOCK_FREQUENCY;         /* one second */
    uint32_t st;
    do { st = audio_mbox(AUDIO_MBOX_STATE); } while (st != ST_TONE && st != AUDIO_ST_TRAP && now() < end);
    if (st == AUDIO_ST_TRAP) {
        printf("  the audio core trapped: mcause=%lu mepc=0x%08lx mtval=0x%08lx\n", (unsigned long)audio_mbox(5),
               (unsigned long)audio_mbox(6), (unsigned long)audio_mbox(7));
        return 1;
    }
    if (st != ST_TONE) { printf("  no answer (state 0x%08lx)\n", (unsigned long)st); return 1; }
    printf("  self-tests: pass bits 0x%02lx, fail bits 0x%02lx\n",
           (unsigned long)audio_mbox(2), (unsigned long)audio_mbox(3));
    printf("  cycles: 16-tap FIR x256 %lu, envelope mix x256 %lu, 256 SDRAM loads %lu\n",
           (unsigned long)audio_mbox(5), (unsigned long)audio_mbox(6), (unsigned long)audio_mbox(7));

    printf("  playing 440 Hz\n");
    for (;;) {
        uint64_t t = now() + (uint64_t)CONFIG_CLOCK_FREQUENCY * 5u;
        while (now() < t) { }
        printf("  %lu samples pushed\n", (unsigned long)audio_mbox(4));
    }
}
