// Verilator test bench for rtl/audio/AudioCore.v (VexRiscvAudio + IMEM/DMEM/
// CTRL + bus glue). The bench plays the SoC:
//   * through the slave port it loads lang/c/audio/build/audio_{imem,dmem}.bin,
//     sets MBOX1 = SDRAM scratch address and RUN = 1 -- exactly what
//     lang/c/game/audio_load.c does on hardware;
//   * on the master port it answers SDRAM (a word map, SDRAM_WAIT extra
//     cycles per access) and the APF audio CSRs (a 4096-entry FIFO that
//     drains one sample every SYS_HZ / 48 kHz cycles (1190 at 57.12 MHz) while
//     playback_en is set).
// Checks: self-test bits, the first 1000 tone samples against the table,
// the SDRAM pattern, then that `div` traps (no divider) and is reported.
//
//   AUDIO_IMEM=... AUDIO_DMEM=... CSR_APF_AUDIO_BASE_OFS=0x0 ./obj_dir/tb_audio
#include "VAudioCore.h"
#include "verilated.h"
// The SoC's sys clock (litex/analogue_pocket.py CLOCK_SPEED)
static const uint64_t SYS_HZ = 57120000;
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>
#include <deque>

static VAudioCore *dut;
static uint64_t g_cyc = 0;

static std::map<uint32_t, uint32_t> sdram;      // byte address (word aligned) -> word
static int      sdram_wait = 2;
static uint32_t apf_base   = 0xF0000000u;       // CSR_APF_AUDIO_BASE
static std::deque<uint32_t> fifo;
static bool     playback = false;
static uint64_t next_drain = 0;
static std::vector<uint32_t> pushed;            // every sample written to OUT
static uint64_t ext_accesses = 0, err_accesses = 0;

// master-port slave model state
static int m_busy = 0;

static void tick_models()
{
    // Called with the clock low, before the rising edge: sample the master
    // request and drive ack/data for this cycle.
    dut->m_ack = 0;
    dut->m_err = 0;
    if (dut->m_cyc && dut->m_stb) {
        if (m_busy < sdram_wait) { m_busy++; return; }
        m_busy = 0;
        uint32_t a = (uint32_t)dut->m_adr << 2;
        ext_accesses++;
        if (a >= 0x40000000u && a < 0x44000000u) {
            uint32_t &w = sdram[a];
            if (dut->m_we) {
                for (int b = 0; b < 4; b++)
                    if (dut->m_sel & (1 << b))
                        w = (w & ~(0xFFu << (8 * b))) | (dut->m_dat_w & (0xFFu << (8 * b)));
            } else dut->m_dat_r = w;
            dut->m_ack = 1;
        } else if (a >= apf_base && a < apf_base + 0x10) {
            uint32_t o = a - apf_base;
            if (dut->m_we) {
                if (o == 0x0) { if (fifo.size() < 4095) fifo.push_back(dut->m_dat_w); pushed.push_back(dut->m_dat_w); }
                if (o == 0x4) playback = dut->m_dat_w & 1;
                if (o == 0x8 && (dut->m_dat_w & 1)) fifo.clear();
            } else {
                dut->m_dat_r = o == 0x4 ? (uint32_t)playback : o == 0xC ? (uint32_t)fifo.size() : 0;
            }
            dut->m_ack = 1;
        } else {
            printf("  !! unmapped master access 0x%08x (%s) at cycle %llu\n", a, dut->m_we ? "W" : "R",
                   (unsigned long long)g_cyc);
            err_accesses++;
            dut->m_err = 1;
        }
    }
}

static void step()
{
    tick_models();
    dut->clk = 0; dut->eval();
    dut->clk = 1; dut->eval();
    g_cyc++;
    if (playback && g_cyc >= next_drain) {
        if (!fifo.empty()) fifo.pop_front();
        next_drain = g_cyc + SYS_HZ / 48000;
    }
}

// Wishbone classic write/read through the slave port.
static void s_write(uint32_t addr, uint32_t v)
{
    dut->s_adr = addr >> 2; dut->s_dat_w = v; dut->s_sel = 0xF; dut->s_we = 1;
    dut->s_cyc = 1; dut->s_stb = 1;
    do { step(); } while (!dut->s_ack);
    dut->s_cyc = 0; dut->s_stb = 0; dut->s_we = 0;
    step();
}
static uint32_t s_read(uint32_t addr)
{
    dut->s_adr = addr >> 2; dut->s_sel = 0xF; dut->s_we = 0;
    dut->s_cyc = 1; dut->s_stb = 1;
    do { step(); } while (!dut->s_ack);
    uint32_t v = dut->s_dat_r;
    dut->s_cyc = 0; dut->s_stb = 0;
    step();
    return v;
}

static std::vector<uint32_t> load_words(const char *path)
{
    std::vector<uint32_t> w;
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    uint8_t b[4];
    size_t n;
    while ((n = fread(b, 1, 4, f)) > 0) {
        for (size_t i = n; i < 4; i++) b[i] = 0;
        w.push_back(b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24);
    }
    fclose(f);
    return w;
}

static const uint32_t IMEM = 0x80000000u, DMEM = 0x80002000u, CTRL = 0x80004000u;
static uint32_t mbox(int i) { return s_read(CTRL + 8 + 4 * i); }

static int g_fail = 0;
static void check(const char *name, bool ok, const char *fmt = "", unsigned long a = 0, unsigned long b = 0)
{
    char d[160]; snprintf(d, sizeof d, fmt, a, b);
    printf("[%-40s] %s %s\n", name, ok ? "PASS" : "FAIL", d);
    if (!ok) g_fail++;
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    dut = new VAudioCore;
    const char *imem_path = getenv("AUDIO_IMEM") ? getenv("AUDIO_IMEM") : "../../lang/c/audio/build/main/audio_imem.bin";
    const char *dmem_path = getenv("AUDIO_DMEM") ? getenv("AUDIO_DMEM") : "../../lang/c/audio/build/main/audio_dmem.bin";
    if (getenv("SDRAM_WAIT")) sdram_wait = atoi(getenv("SDRAM_WAIT"));
    if (getenv("APF_AUDIO_BASE")) apf_base = strtoul(getenv("APF_AUDIO_BASE"), 0, 0);

    dut->rst = 1;
    for (int i = 0; i < 8; i++) step();
    dut->rst = 0;
    step();

    // Load exactly as the game CPU does: core in reset, IMEM, DMEM, verify.
    check("core held in reset after sys reset", s_read(CTRL) == 0);
    auto im = load_words(imem_path), dm = load_words(dmem_path);
    for (size_t i = 0; i < im.size(); i++) s_write(IMEM + 4 * i, im[i]);
    for (size_t i = 0; i < dm.size(); i++) s_write(DMEM + 4 * i, dm[i]);
    int bad = 0;
    for (size_t i = 0; i < im.size(); i++) bad += s_read(IMEM + 4 * i) != im[i];
    for (size_t i = 0; i < dm.size(); i++) bad += s_read(DMEM + 4 * i) != dm[i];
    check("IMEM/DMEM load read back", bad == 0, "(%lu words, %lu bad)", im.size() + dm.size(), bad);

    const uint32_t scratch = 0x42000000u;
    s_write(CTRL + 8 + 4 * 1, scratch);
    uint64_t t_run = g_cyc;
    s_write(CTRL, 1);

    // Wait for the tone state (tests done).
    uint32_t st = 0;
    while (g_cyc - t_run < 2000000) {
        for (int i = 0; i < 200; i++) step();
        st = mbox(0);
        if (st == 0xA0D10002u || st == 0xA0D1DEADu) break;
    }
    check("core reached the tone loop", st == 0xA0D10002u, "(state 0x%08lx after %lu cycles)", st, g_cyc - t_run);
    if (st == 0xA0D1DEADu)
        printf("  trap: mcause=0x%08x mepc=0x%08x mtval=0x%08x\n", mbox(5), mbox(6), mbox(7));
    uint32_t pass = mbox(2), fail = mbox(3);
    const char *names[6] = { "T0 multiply", "T1 DMEM sub-word", "T2 SDRAM word/byte", "T3 APF FIFO CSRs", "T4 cycle counter",
                             "T5 DSP instructions (CFU)" };
    for (int t = 0; t < 6; t++)
        check(names[t], (pass >> t & 1) && !(fail >> t & 1));
    printf("  perf: FIR 16x256 %u cycles (%.1f/tap), mix 256 %u cycles (%.1f/sample), 256 SDRAM loads %u cycles (%.1f each, SDRAM_WAIT=%d)\n",
           mbox(5), mbox(5) / 4096.0, mbox(6), mbox(6) / 256.0, mbox(7), mbox(7) / 256.0, sdram_wait);

    // SDRAM pattern the core wrote.
    bad = 0;
    for (uint32_t i = 0; i < 64; i++) bad += sdram[scratch + 4 * i] != i * 0x9E3779B9u;
    check("SDRAM pattern visible from the SoC side", bad == 0 && sdram[scratch + 256] == 0x11ABCDEFu);

    // Let the tone run for 100 ms of sim time and check the stream.
    while (fifo.size() < 3000 && g_cyc - t_run < 20000000) step();   // past the initial fill
    uint64_t t0 = g_cyc;
    size_t p0 = pushed.size();
    while (g_cyc - t0 < SYS_HZ / 10) step();
    double rate = (pushed.size() - p0) / 0.1;
    check("FIFO kept fed (48 kHz)", fabs(rate - 48000) < 2000 && !fifo.empty(), "(%lu samples/s, fill %lu)",
          (unsigned long)rate, fifo.size());
    // First tone samples: T3's 10 test pushes come first, then the tone
    // (whose own first sample is also 0).
    size_t first = 10;
    const uint32_t step_ph = 39370534u;
    uint32_t ph = 0;
    bad = 0;
    for (size_t i = 0; i < 1000 && first + i < pushed.size(); i++, ph += step_ph) {
        int16_t want = (int16_t)lround(8000 * sin(2 * M_PI * (ph >> 26) / 64.0));
        uint32_t w = ((uint32_t)(uint16_t)want << 16) | (uint16_t)want;
        bad += pushed[first + i] != w;
    }
    check("440 Hz tone samples match the table", bad == 0, "(%lu mismatches of 1000)", bad);
    check("no unmapped bus accesses", err_accesses == 0, "(%lu)", err_accesses);

    // No divider: `div x1,x2,x3` must trap, and the handler must report it.
    s_write(CTRL, 0);
    s_write(IMEM + 0, 0x023140B3u);   // div x1, x2, x3
    s_write(CTRL + 8, 0);
    s_write(CTRL, 1);
    for (int i = 0; i < 2000; i++) step();
    uint32_t tst = mbox(0), mc = mbox(5), me = mbox(6);
    check("div traps (illegal instruction)", tst == 0xA0D1DEADu && mc == 2 && me == IMEM,
          "(mcause %lu, mepc 0x%08lx)", mc, me);

    printf(g_fail ? "SOME FAILED (%d)\n" : "ALL PASS\n", g_fail);
    delete dut;
    return g_fail ? 1 : 0;
}
