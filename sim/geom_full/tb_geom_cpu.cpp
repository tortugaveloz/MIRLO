// Incremental "add every component" simulation, step 1: the REAL
// VexRiscvGeom RISC-V core (rtl/geom/VexRiscvGeom.v) executing the REAL
// compiled lang/c/geom/build/geom.bin, issuing REAL CFU custom instructions
// to the REAL Vpu4DFixed.v Verilog (rtl/vpu/Vpu4DFixed.v) -- the piece the earlier
// CFU-only / geom_pipeline.c-on-x86 simulations could NOT exercise. Only the
// Wishbone memory system (ROM/RAM/mailbox/a GDL-holding SDRAM region) is a
// C++ behavioral model here, not real RTL -- that's deliberately the next
// increment, not this one.
//
// Protocol/addresses replicated exactly from:
//   lang/c/geom/main.c + mailbox.h  (the real mailbox doorbell handshake)
//   litex/analogue_pocket.py        (GEOM_ROM_BASE/GEOM_RAM_BASE/CSR_BASE)
//   lang/c/geom/build/generated_csr_addrs.h (exact mailbox CSR offsets)
//
// Usage: tb_geom_cpu <geom.bin> <gdl.bin> [gdl_load_addr_hex]
//   Loads geom.bin at GEOM_ROM_BASE, gdl.bin at gdl_load_addr (default
//   0x41000000, matching real hardware's GDL_RING_BASE), rings the mailbox
//   doorbell with that address once the CPU signals it's waiting (heartbeat
//   0xB0000040), and watches for the "DL walk complete" heartbeat (0xB0000070)
//   or a timeout. Every word sent to MRDP's command FIFO is captured to
//   geom_cmd_stream.bin (check_host_vs_rtl.sh compares it with the host's).
#include <verilated.h>
#include "Vgeom_cpu_top.h"
#include "Vgeom_cpu_top__Syms.h"
// Optional instrumentation (see README.md):
//   PCPROF=<file>  histogram of the writeback-stage PC, one "pc cycles" line
//                  per address -- tools/symprof.py folds it per function.
//   CFUTRACE=1     print the first 400 CFU bus transactions (function_id as
//                  the CFU actually receives it, operands, results).
static std::map<uint32_t, uint64_t> g_pchist; static bool g_prof, g_cfutrace;
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <map>
#include <string>

static Vgeom_cpu_top *dut;
static vluint64_t g_cycle = 0;

// ---- address map (byte addresses) ----------------------------------------
#ifndef TB_ROM_SIZE
#define TB_ROM_SIZE 0x8000u   /* litex/analogue_pocket.py GEOM_ROM_SIZE */
#endif
#ifndef TB_RAM_BASE
#define TB_RAM_BASE 0x20008000u
#endif
constexpr uint32_t GEOM_ROM_BASE = 0x20000000u, GEOM_ROM_SIZE = TB_ROM_SIZE;
constexpr uint32_t GEOM_RAM_BASE = TB_RAM_BASE, GEOM_RAM_SIZE = 0x4000u;
constexpr uint32_t CSR_BASE = 0xf0000000u;
// Mailbox / MRDP CSR offsets come from the LiteX build's csr.h (build.sh
// greps them into -D flags): removing a LiteX peripheral shifts the whole CSR
// map, and a stale copy here makes the tb silently drop every mailbox write.
#if !defined(TB_MB_OFS) || !defined(TB_MRDP_OFS)
#error "build with build.sh (TB_MB_OFS/TB_MRDP_OFS from csr.h)"
#endif
constexpr uint32_t MB_GAME_MSG  = CSR_BASE + TB_MB_OFS + 0x00u;
constexpr uint32_t MB_GAME_KICK = CSR_BASE + TB_MB_OFS + 0x04u;
constexpr uint32_t MB_GEOM_ACK  = CSR_BASE + TB_MB_OFS + 0x08u;
constexpr uint32_t MB_GEOM_MSG  = CSR_BASE + TB_MB_OFS + 0x0cu;
constexpr uint32_t MB_GEOM_KICK = CSR_BASE + TB_MB_OFS + 0x10u;
constexpr uint32_t MB_GAME_ACK  = CSR_BASE + TB_MB_OFS + 0x14u;
constexpr uint32_t MB_STATUS    = CSR_BASE + TB_MB_OFS + 0x18u;
constexpr uint32_t MRDP_CMD_DATA   = CSR_BASE + TB_MRDP_OFS + 0x00u;
// MRDP (litex/mrdp.py): the stream runs through the bit-exact C model as the
// core writes it, so load_count/sync_count are what the hardware would show
// (the geom's texture staging ring waits on load_count) and TB_FB=<file>
// dumps the frame the real geom CPU drew.
#include "../../lang/c/mrdp/mrdp.h"
#include "../../lang/c/mrdp/mrdp_frame.h"
constexpr uint32_t MRDP_CMD_STATUS  = CSR_BASE + TB_MRDP_OFS + 0x04u;
constexpr uint32_t MRDP_SYNC_COUNT = CSR_BASE + TB_MRDP_OFS + 0x0cu;
constexpr uint32_t MRDP_LOAD_COUNT = CSR_BASE + TB_MRDP_OFS + 0x10u;
constexpr uint32_t MRDP_STATUS     = CSR_BASE + TB_MRDP_OFS + 0x14u;
constexpr uint32_t TB_FB = 0x40C00000u, TB_ZB = 0x40D01000u;
static mrdp_t s_mrdp;

static std::vector<uint8_t> s_rom(GEOM_ROM_SIZE, 0);
static std::vector<uint8_t> s_ram(GEOM_RAM_SIZE, 0);
static uint32_t s_gdl_base = 0x41000000u;
// The rest of SDRAM, plain memory: the texture staging ring, the geom's
// decoded-texture cache and its table live there. Before this existed every
// such access read 0 and dropped its writes (the unmapped fallback below), so
// nothing the geom core kept in SDRAM could be observed. SDRAM_WAIT=<n> holds
// each SDRAM beat n extra cycles (default 0: the zero-wait model the cycle
// counts so far were taken with).
constexpr uint32_t SDRAM_BASE = 0x40000000u, SDRAM_SIZE = 0x04000000u;
static std::vector<uint8_t> s_sdram(SDRAM_SIZE, 0);
static int s_sdram_wait;
// BUS_WAIT=<n>: n extra cycles on EVERY access, ROM and RAM included. On the
// SoC the geom core's ROM, RAM and SDRAM all sit behind the shared main
// Wishbone interconnect (analogue_pocket.py: bus.add_master("geom_cpu")), so
// I-cache refills and the write-through D-cache's stores pay its latency and
// arbitration against the game CPU; this harness acks in zero cycles.
static int s_bus_wait;
// LOCAL_WAIT=<n>: the wait for geom ROM/RAM instead of BUS_WAIT -- a private
// bus to its own memories (what-if; default = BUS_WAIT, the shared bus).
static int s_local_wait = -1;
// Per-walk bus transaction counts, by bus / direction / region.
static uint64_t s_bc[2][2][4];   // [ibus/dbus][read/write][rom, ram, sdram, other]
// Texture-content checks. TEXFILL=1: an unmapped read returns a hash of its
// address instead of 0, so a GDL dumped on the host (texture pointers that
// mean nothing here) still decodes to distinct, repeatable texels.
// TEXHASH=<file>: every word written to MRDP_CMD_DATA that is a staging-ring
// or texture-cache page address logs a hash of the 2 KB there at that moment
// -- what MRDP would read. Two builds that must stream the same
// textures must log the same hashes, whatever pages they used.
static bool s_texfill; static FILE *s_texhash;
// SDRAM_JUNK=1: the real boot. The geom core leaves reset with the FPGA,
// before the BIOS has initialised SDRAM: its writes before then are lost and
// SDRAM holds whatever it powered up with. Model: SDRAM starts filled with
// junk and ignores writes until the first doorbell. A geom build that set up
// SDRAM state in geom_reset() (the texture cache's table did) looks fine
// without this and broken on hardware.
static bool s_sdram_junk, s_sdram_live = true;
static std::vector<uint8_t> s_gdl; // sized to whatever's loaded

static uint32_t s_mb_status = 0; // bit0 = geom_pending
static uint32_t s_mb_game_msg = 0;
static uint32_t s_mb_geom_msg = 0;
static bool s_geom_kicked_done = false;

static std::vector<uint32_t> s_cmd_stream; // words written to MRDP_CMD_DATA, in order

static uint32_t last_heartbeat = 0;

static bool in_range(uint32_t a, uint32_t base, uint32_t size) { return a >= base && a < base + size; }

// One Wishbone classic-cycle access, zero-wait-state (combinational ACK).
// adr is a WORD address (addressing="word" per analogue_pocket.py).
// Writes a word to `dst` respecting Wishbone SEL byte-enables -- a real
// Wishbone slave only updates the byte lanes SEL asks for; a partial (e.g.
// sb-generated) write must NOT touch the other 3 bytes. Discovered missing
// here 2026-09-12 while chasing a real-CPU-vs-x86-native divergence: this
// harness previously did a blind 4-byte memcpy regardless of SEL, so any
// byte-store (sb) -- whose bus-level DAT_MOSI replicates the stored byte
// across all 32 bits, real VexRiscv/Wishbone behavior, SEL picks the lane --
// clobbered the other 3 bytes of the target word with that same replicated
// byte, and the LAST byte-store in a sequence "won", leaving the whole word
// equal to its own MSB replicated 4x. Confirmed via bus-level tracing: the
// data *arriving* over Wishbone was always correct; only the (SEL-blind)
// write-back here was wrong. This was a test-harness bug, not a real
// firmware/RTL bug -- real hardware's Wishbone RAM controller respects SEL.
static void sel_write(uint8_t *dst, uint32_t dat_w, uint8_t sel) {
    for (int i = 0; i < 4; i++) {
        if (sel & (1u << i)) dst[i] = (uint8_t)(dat_w >> (i * 8));
    }
}

static void wb_access(bool cyc, bool stb, bool we, uint32_t adr_word, uint32_t dat_w,
                      uint8_t sel, uint32_t &dat_r, uint8_t &ack, const char *busname) {
    ack = 0;
    dat_r = 0;
    if (!(cyc && stb)) return;
    uint32_t addr = adr_word << 2;
    {
        static bool pend[2];
        int b = busname[0] == 'd';
        if (!pend[b]) {
            int reg = in_range(addr, GEOM_ROM_BASE, GEOM_ROM_SIZE) ? 0 : in_range(addr, GEOM_RAM_BASE, GEOM_RAM_SIZE) ? 1
                    : (addr >= 0x40000000u && addr < 0x44000000u) ? 2 : 3;
            s_bc[b][we ? 1 : 0][reg]++;
        }
        pend[b] = true;   // cleared below once acked, so a waited access counts once
        int wait = s_bus_wait;
        if (s_local_wait >= 0 && (in_range(addr, GEOM_ROM_BASE, GEOM_ROM_SIZE) || in_range(addr, GEOM_RAM_BASE, GEOM_RAM_SIZE)))
            wait = s_local_wait;
        if (wait) {
            static int bw[2];
            if (bw[b] < wait) { bw[b]++; return; }
            bw[b] = 0;
        }
        pend[b] = false;
    }
    if (false) {
        static int bw[2];
        int b = busname[0] == 'd';
        if (bw[b] < s_bus_wait) { bw[b]++; return; }
        bw[b] = 0;
    }

    if (in_range(addr, GEOM_ROM_BASE, GEOM_ROM_SIZE)) {
        uint32_t off = addr - GEOM_ROM_BASE;
        // ROM B (0x4000 on) is on the core's iBus only (litex/analogue_pocket.py)
        if (busname[0] == 'd' && off >= 0x4000u) {
            static int nb; if (nb++ < 5) fprintf(stderr, "  [WARN] dBus read of fetch-only ROM B at 0x%08X\n", addr);
        }
        if (we) { /* ROM: ignore writes (shouldn't happen) */ }
        else memcpy(&dat_r, &s_rom[off], 4);
        ack = 1; return;
    }
    if (in_range(addr, GEOM_RAM_BASE, GEOM_RAM_SIZE)) {
        uint32_t off = addr - GEOM_RAM_BASE;
        if (we) sel_write(&s_ram[off], dat_w, sel);
        else memcpy(&dat_r, &s_ram[off], 4);
        ack = 1; return;
    }
    if (in_range(addr, s_gdl_base, (uint32_t)s_gdl.size())) {
        uint32_t off = addr - s_gdl_base;
        if (we) sel_write(&s_gdl[off], dat_w, sel); // not expected, but harmless
        else memcpy(&dat_r, &s_gdl[off], 4);
        ack = 1; return;
    }
    if (in_range(addr, SDRAM_BASE, SDRAM_SIZE)) {
        static int waited[2];
        int b = busname[0] == 'd';
        if (waited[b] < s_sdram_wait) { waited[b]++; return; }
        waited[b] = 0;
        uint32_t off = addr - SDRAM_BASE;
        if (we && s_sdram_live) sel_write(&s_sdram[off], dat_w, sel);
        else memcpy(&dat_r, &s_sdram[off], 4);
        ack = 1; return;
    }
    if (addr == MB_STATUS)   { if (!we) dat_r = s_mb_status; ack = 1; return; }
    if (addr == MB_GAME_MSG) { if (!we) dat_r = s_mb_game_msg; ack = 1; return; }
    if (addr == MB_GEOM_ACK) { if (we && (dat_w & 1)) s_mb_status &= ~1u; ack = 1; return; }
    if (addr == MB_GEOM_MSG) {
        if (we) {
            s_mb_geom_msg = dat_w;
            if (s_mb_geom_msg != last_heartbeat) {
                printf("  [geom heartbeat] 0x%08X\n", s_mb_geom_msg);
                last_heartbeat = s_mb_geom_msg;
            }
        } else dat_r = s_mb_geom_msg;
        ack = 1; return;
    }
    if (addr == MB_GEOM_KICK) { if (we && (dat_w & 1)) s_geom_kicked_done = true; ack = 1; return; }
    if (addr == MRDP_CMD_STATUS) { if (!we) dat_r = 0; ack = 1; return; } // never "full"
    if (addr == MRDP_SYNC_COUNT) { if (!we) dat_r = s_mrdp.sync_count; ack = 1; return; }
    if (addr == MRDP_LOAD_COUNT) { if (!we) dat_r = s_mrdp.load_count; ack = 1; return; }
    if (addr == MRDP_STATUS)     { if (!we) dat_r = 1u | (s_mrdp.unknown_ops << 1); ack = 1; return; }
    if (addr == MRDP_CMD_DATA) {
        if (we) s_cmd_stream.push_back(dat_w);
        if (we) mrdp_push(&s_mrdp, dat_w);
        if (we && s_texhash && ((dat_w >= 0x41200000u && dat_w < 0x41300000u) ||
                                (dat_w >= 0x41400000u && dat_w < 0x41600000u)) && (dat_w & 0x7FFu) == 0) {
            uint64_t h = 1469598103934665603ull;
            for (uint32_t i = 0; i < 2048; i++) { h ^= s_sdram[dat_w - SDRAM_BASE + i]; h *= 1099511628211ull; }
            fprintf(s_texhash, "%016llx\n", (unsigned long long)h);
        }
        ack = 1; return;
    }

    // Unknown address -- ack anyway (return 0) so the CPU never wedges on a
    // stray access, but flag it loudly since it's evidence of something this
    // memory model doesn't understand yet.
    if (s_texfill && !we) { dat_r = (addr * 2654435761u) ^ (addr >> 7); ack = 1; return; }
    static std::map<uint32_t,int> warned;
    if (warned[addr]++ < 3) {
        fprintf(stderr, "  [WARN] %s access to unmapped addr 0x%08X (we=%d dat=0x%08X)\n",
                busname, addr, we, dat_w);
    }
    ack = 1;
}

static uint32_t s_trace_lo = 0xFFFFFFFFu, s_trace_hi = 0;

static void tick() {
#ifdef GEOM_IBUS_SIMPLE
    // iBus straight from the ROM's own read port (GEOM_IBUS=simple core): a
    // command every cycle, its instruction the next -- an M10K's registered read
    static bool ib_pend; static uint32_t ib_inst;
    dut->iBus_cmd_ready = 1;
    dut->iBus_rsp_valid = ib_pend;
    dut->iBus_rsp_payload_inst = ib_inst;
    dut->iBus_rsp_payload_error = 0;
    s_bc[0][0][0] += ib_pend;
#else
    // iBus
    uint8_t iack; uint32_t idat;
    wb_access(dut->iBusWishbone_CYC, dut->iBusWishbone_STB, dut->iBusWishbone_WE,
              dut->iBusWishbone_ADR, dut->iBusWishbone_DAT_MOSI, dut->iBusWishbone_SEL,
              idat, iack, "iBus");
    dut->iBusWishbone_ACK = iack;
    dut->iBusWishbone_DAT_MISO = idat;
    dut->iBusWishbone_ERR = 0;
#endif

#ifdef GEOM_DTCM
    // the geom RAM on the core's tightly coupled port: an M10K, registered
    // read, its output held while the port is idle
    static uint32_t tcm_q;
    dut->dTcm_read_data = tcm_q;
#endif
    uint8_t dack; uint32_t ddat;
    wb_access(dut->dBusWishbone_CYC, dut->dBusWishbone_STB, dut->dBusWishbone_WE,
              dut->dBusWishbone_ADR, dut->dBusWishbone_DAT_MOSI, dut->dBusWishbone_SEL,
              ddat, dack, "dBus");
    dut->dBusWishbone_ACK = dack;
    dut->dBusWishbone_DAT_MISO = ddat;
    dut->dBusWishbone_ERR = 0;

    if (s_trace_lo <= s_trace_hi && dut->dBusWishbone_CYC && dut->dBusWishbone_STB) {
        uint32_t byteaddr = dut->dBusWishbone_ADR << 2;
        if (byteaddr >= s_trace_lo && byteaddr <= s_trace_hi) {
            printf("  [TRACE dBus] cyc=%llu addr=0x%08X we=%u sel=%u cti=%u bte=%u dat_mosi=0x%08X dat_miso=0x%08X ack=%u\n",
                   (unsigned long long)g_cycle, byteaddr, dut->dBusWishbone_WE, dut->dBusWishbone_SEL,
                   dut->dBusWishbone_CTI, dut->dBusWishbone_BTE, dut->dBusWishbone_DAT_MOSI, ddat, dack);
        }
    }

    /* the CFU's PUSH port writes into MRDP's command FIFO exactly like
     * the core's CSR stores did: into the same captured stream, in order */
    dut->su_out_ready = 1;
    dut->clk = 0; dut->eval();
    if (dut->su_out_valid && dut->su_out_ready) {
        s_cmd_stream.push_back(dut->su_out_data);
        mrdp_push(&s_mrdp, dut->su_out_data);
    }
#ifdef GEOM_IBUS_SIMPLE
    ib_pend = dut->iBus_cmd_valid;               // accepted at this edge (ready = 1)
    if (ib_pend) {
        uint32_t off = (uint32_t)dut->iBus_cmd_payload_pc - GEOM_ROM_BASE;
        ib_inst = off + 4 <= GEOM_ROM_SIZE ? (uint32_t)s_rom[off] | (uint32_t)s_rom[off + 1] << 8
                                              | (uint32_t)s_rom[off + 2] << 16 | (uint32_t)s_rom[off + 3] << 24 : 0u;
    }
#endif
#ifdef GEOM_DTCM
    if (dut->dTcm_enable && dut->dTcm_write_enable && dut->rootp->vlSymsp->TOP__geom_cpu_top__cpu.execute_arbitration_isFlushed) {
        static int nf; if (nf++ < 5) fprintf(stderr, "  [TCM] store while execute is flushed, cyc=%llu addr=%08x\n", (unsigned long long)g_cycle, (unsigned)dut->dTcm_address);
    }
    if (dut->dTcm_enable) {
        uint32_t off = ((uint32_t)dut->dTcm_address - GEOM_RAM_BASE) & (GEOM_RAM_SIZE - 1) & ~3u;
        memcpy(&tcm_q, &s_ram[off], 4);
        if (dut->dTcm_write_enable) { sel_write(&s_ram[off], dut->dTcm_write_data, dut->dTcm_write_mask); s_bc[1][1][1]++; }
        else s_bc[1][0][1]++;
    }
#endif
    dut->clk = 1; dut->eval();
    { static int ntr = 0; auto &T = dut->rootp->vlSymsp->TOP__geom_cpu_top;
      if (g_cfutrace && ntr < 400) {
        if (T.cfu_cmd_valid && T.cfu_cmd_ready) { printf("  CFU cmd fid=%03x in0=%08x in1=%08x cyc=%llu\n", T.cfu_cmd_fid, T.cfu_cmd_in0, T.cfu_cmd_in1, (unsigned long long)g_cycle); ntr++; }
        if (T.cfu_rsp_valid && T.cfu_rsp_ready) { printf("  CFU rsp out=%08x cyc=%llu\n", T.cfu_rsp_out0, (unsigned long long)g_cycle); ntr++; } } }
    if (g_prof) g_pchist[dut->rootp->vlSymsp->TOP__geom_cpu_top__cpu.memory_to_writeBack_PC]++;
    g_cycle++;
}

static uint16_t tb_rd16(void *, uint32_t a) { uint16_t v; memcpy(&v, &s_sdram[a - SDRAM_BASE], 2); return v; }
static void tb_wr16(void *, uint32_t a, uint16_t v) { memcpy(&s_sdram[a - SDRAM_BASE], &v, 2); }
int main(int argc, char **argv) {
    mrdp_init(&s_mrdp, nullptr, tb_rd16, tb_wr16);
    Verilated::commandArgs(argc, argv);
    g_prof = getenv("PCPROF") != nullptr;
    g_cfutrace = getenv("CFUTRACE") != nullptr;
    if (const char *w = getenv("SDRAM_WAIT")) s_sdram_wait = atoi(w);
    if (const char *w = getenv("BUS_WAIT")) s_bus_wait = atoi(w);
    if (const char *w = getenv("LOCAL_WAIT")) s_local_wait = atoi(w);
    s_texfill = getenv("TEXFILL") != nullptr;
    if (const char *th = getenv("TEXHASH")) s_texhash = fopen(th, "w");
    if (getenv("SDRAM_JUNK")) {
        s_sdram_junk = true; s_sdram_live = false;
        uint32_t x = 0x9E3779B9u;
        for (size_t i = 0; i < s_sdram.size(); i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; s_sdram[i] = (uint8_t)x; }
    }
    if (argc < 3) { fprintf(stderr, "usage: %s <geom.bin> <gdl.bin> [gdl_addr_hex]\n", argv[0]); return 2; }
    if (const char *lo = getenv("TRACE_LO")) s_trace_lo = (uint32_t)strtoul(lo, nullptr, 16);
    if (const char *hi = getenv("TRACE_HI")) s_trace_hi = (uint32_t)strtoul(hi, nullptr, 16);

    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    size_t n = fread(s_rom.data(), 1, s_rom.size(), f);
    fclose(f);
    printf("loaded %zu bytes of geom firmware into ROM @0x%08X\n", n, GEOM_ROM_BASE);

    f = fopen(argv[2], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[2]); return 2; }
    fseek(f, 0, SEEK_END); long gdl_len = ftell(f); fseek(f, 0, SEEK_SET);
    s_gdl.resize(((size_t)gdl_len + 4095) & ~size_t(4095)); // pad to a page, plenty of room
    fread(s_gdl.data(), 1, gdl_len, f);
    fclose(f);
    if (argc > 3) s_gdl_base = (uint32_t)strtoul(argv[3], nullptr, 16);
    printf("loaded %ld bytes of GDL @0x%08X (padded buffer %zu bytes)\n", gdl_len, s_gdl_base, s_gdl.size());

    dut = new Vgeom_cpu_top;
    dut->reset = 1;
    for (int i = 0; i < 8; i++) tick();
    dut->reset = 0;

    bool kicked = false;
    uint64_t guard_cycles = 0;
    // REPEAT=<n>: walk the same GDL n times, as consecutive frames, and report
    // each walk's kick->done cycles. What persists across frames on the geom
    // core (the decoded-texture cache) only shows from the second walk on.
    // The command stream written out is the last walk's.
    int repeat = getenv("REPEAT") ? atoi(getenv("REPEAT")) : 1, run = 0;
    if (repeat < 1) repeat = 1;
    uint64_t kick_cycle = 0;
    const uint64_t MAX_CYCLES = 20000000ull * (uint64_t)repeat; // generous -- real boot+one DL walk should be far fewer
    while (g_cycle < MAX_CYCLES) {
        tick();
        guard_cycles++;
        // Once the geom core reports it's waiting for the doorbell
        // (0xB0000040, main.c's HB right before mbox_wait_kick()), ring it --
        // exactly what the game CPU's frame_submit()/geom kick would do.
        if (!kicked && last_heartbeat == 0xB0000040u) {
            printf("  --> ringing mailbox doorbell with dl_base=0x%08X\n", s_gdl_base);
            s_mb_game_msg = s_gdl_base;
            s_mb_status |= 1u; // geom_pending
            kicked = true;
            kick_cycle = g_cycle;
            memset(s_bc, 0, sizeof s_bc);
            if (getenv("PCPROF_LAST")) g_pchist.clear();   // profile only the walk that ends last
            if (!s_sdram_live) {   // what the game CPU's frame_init() writes: GEOM_CFG = 0
                s_sdram_live = true;
                memset(&s_sdram[0x41300000u - SDRAM_BASE], 0, 32);
            }
            s_cmd_stream.clear();
            {   // the frame's clears, as the game CPU's GDL / hostreplay put them
                uint32_t w[MRDP_FRAME_CLEAR_WORDS];
                unsigned nw = mrdp_frame_clear(w, TB_FB, TB_ZB, 268, 240, 0x404040u);
                for (unsigned i = 0; i < nw; i++) mrdp_push(&s_mrdp, w[i]);
            }
        }
        if (s_geom_kicked_done) {
            printf("  run %d: %llu cycles kick->done\n", run + 1, (unsigned long long)(g_cycle - kick_cycle));
            printf("    bus: ibus rd rom %llu ram %llu sdram %llu | dbus rd rom %llu ram %llu sdram %llu other %llu | dbus wr ram %llu sdram %llu other %llu\n",
                   (unsigned long long)s_bc[0][0][0], (unsigned long long)s_bc[0][0][1], (unsigned long long)s_bc[0][0][2],
                   (unsigned long long)s_bc[1][0][0], (unsigned long long)s_bc[1][0][1], (unsigned long long)s_bc[1][0][2], (unsigned long long)s_bc[1][0][3],
                   (unsigned long long)s_bc[1][1][1], (unsigned long long)s_bc[1][1][2], (unsigned long long)s_bc[1][1][3]);
            memset(s_bc, 0, sizeof s_bc);
            if (++run >= repeat) break;
            s_geom_kicked_done = false; kicked = false;
            last_heartbeat = 0;   // wait for the next 0xB0000040
        }
    }

    if (!s_geom_kicked_done) {
        printf("\nTIMEOUT after %llu cycles -- geom core never signalled done "
               "(last heartbeat 0x%08X)\n", (unsigned long long)g_cycle, last_heartbeat);
        delete dut;
        return 1;
    }

    printf("\ngeom core signalled done after %llu cycles. last heartbeat 0x%08X\n",
           (unsigned long long)g_cycle, last_heartbeat);
    printf("captured %zu words written to MRDP_CMD_DATA\n", s_cmd_stream.size());

    // SDRAM_DUMP=<file>: 0x41200000..0x41608000 (texture staging ring,
    // texture cache pages and table), so a replay of the captured stream
    // can sample the texels this run decoded.
    if (const char *sd = getenv("SDRAM_DUMP")) {
        FILE *df = fopen(sd, "wb");
        if (df) { fwrite(&s_sdram[0x01200000], 1, 0x00408000, df); fclose(df); }
    }
    printf("mrdp: %llu pixels, %u loads, %u unknown ops\n",
           (unsigned long long)s_mrdp.pixels_drawn, s_mrdp.load_count, s_mrdp.unknown_ops);
    if (const char *fbp = getenv("TB_FB")) {
        FILE *ff = fopen(fbp, "wb");
        if (ff) { fwrite(&s_sdram[TB_FB - SDRAM_BASE], 2, 268 * 240, ff); fclose(ff); }
    }
    FILE *ramout = fopen("geom_ram_dump.bin", "wb");
    if (ramout) { fwrite(s_ram.data(), 1, s_ram.size(), ramout); fclose(ramout); }

    FILE *out = fopen("geom_cmd_stream.bin", "wb");
    if (out) {
        fwrite(s_cmd_stream.data(), 4, s_cmd_stream.size(), out);
        fclose(out);
        printf("wrote geom_cmd_stream.bin (%zu bytes)\n", s_cmd_stream.size() * 4);
    }

    delete dut;
    if (g_prof) { FILE *pf = fopen(getenv("PCPROF"), "w"); for (auto &kv : g_pchist) fprintf(pf, "%08x %llu\n", kv.first, (unsigned long long)kv.second); fclose(pf); }
    return 0;
}
