// MRDP inside the SoC (litex/sim_mrdp.py): the game CPU replays a random
// MRDP command stream (mrdp_cap.h, from mkcap_mrdp.sh) through the cmd_data
// CSR -- the real CSR bus, command FIFO and LiteDRAM native port -- and
// checks the colour and depth images against the C model's hashes.
//
// Memory events (texture data) are written only while MRDP is idle: the
// capture reuses one texture address, and a LOAD TILE still queued would
// otherwise read the next texture's texels.

#include <stdint.h>
#include <stdio.h>

#include <generated/csr.h>
#include <generated/mem.h>
#include <generated/soc.h>
#include <system.h>

#include "mrdp_cap.h"

static uint64_t now(void) { timer0_uptime_latch_write(1); return timer0_uptime_cycles_read(); }

static int wait_idle(const char *where)
{
    uint32_t n = 0;
    while (mrdp_cmd_status_level_read() != 0 || !mrdp_status_idle_read()) {
        if (++n == 20000000u) {
            printf("  [stuck in %s] status=0x%08lx cmd_status=0x%08lx sync=%lu loads=%lu\n", where,
                   (unsigned long)mrdp_status_read(), (unsigned long)mrdp_cmd_status_read(),
                   (unsigned long)mrdp_sync_count_read(), (unsigned long)mrdp_load_count_read());
            return 0;
        }
    }
    return 1;
}

static uint32_t fnv(uint32_t addr, uint32_t n)
{
    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)addr;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

int main(void)
{
    printf("\nMRDP SoC test: %u capture words, %dx%d frame\n", (unsigned)(sizeof cap / 4), CAP_W, CAP_H);
    uint32_t s0 = mrdp_sync_count_read(), words = 0, mems = 0;
    uint64_t t0 = now();
    const uint32_t *p = cap;
    while (*p) {
        uint32_t h = *p++;
        if (h & 0x80000000u) {                    /* a run of command words */
            for (uint32_t i = 0; i < (h & 0xFFFFFFu); i++) {
                while (mrdp_cmd_status_full_read()) { }
                mrdp_cmd_data_write(*p++);
                words++;
            }
        } else {                                  /* memory: nw words at addr */
            uint32_t nw = h & 0xFFFFFFu, addr = *p++;
            if (!wait_idle("mem")) return 1;
            volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)addr;
            for (uint32_t i = 0; i < nw; i++) d[i] = *p++;
            flush_cpu_dcache();
            mems++;
        }
    }
    uint32_t n = 0;
    while (mrdp_sync_count_read() == s0) {
        if (++n == 40000000u) { printf("  [stuck waiting for SYNC FULL]\n"); break; }
    }
    uint64_t t1 = now();
    flush_cpu_dcache();
    uint32_t hfb = fnv(CAP_FB, CAP_W * CAP_H * 2), hz = fnv(CAP_ZB, CAP_W * CAP_H * 2);
    printf("  %lu words, %lu memory blocks, %lu cycles, sync %lu, loads %lu, unknown %lu, dropped %lu\n",
           (unsigned long)words, (unsigned long)mems, (unsigned long)(t1 - t0),
           (unsigned long)mrdp_sync_count_read(), (unsigned long)mrdp_load_count_read(),
           (unsigned long)mrdp_status_unknown_ops_read(), (unsigned long)mrdp_cmd_dropped_read());
    printf("  colour 0x%08lx (model 0x%08lx)  depth 0x%08lx (model 0x%08lx)\n",
           (unsigned long)hfb, (unsigned long)CAP_FB_HASH, (unsigned long)hz, (unsigned long)CAP_Z_HASH);
    printf("MRDP SOC %s\n", hfb == CAP_FB_HASH && hz == CAP_Z_HASH ? "PASS" : "FAIL");
    for (;;) { }
    return 0;
}
