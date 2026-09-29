// MRDP RTL vs the C model (lang/c/mrdp/mrdp.c, the contract).
//   tb_mrdp <capture> [fb_ppm]
// <capture> (e.g. from lang/c/mrdp's test_mrdp_rand): {1, n, words} command
// events and {2, addr, bytes, data} SDRAM writes (decoded textures). Both
// the RTL and the model replay it; then every byte of their SDRAMs must match.
// The RTL's memory is a LiteDRAM-native-style port with random back-pressure
// and read latency.
#include "Vmrdp_top.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>
#include "../../lang/c/mrdp/mrdp.h"   // mrdp.c is built as C++ here too

static const uint32_t BASE = 0x40000000u, SIZE = 64u << 20;
static uint8_t *rtl_mem, *mdl_mem;
static uint16_t mdl_rd(void *, uint32_t a) { uint16_t v; memcpy(&v, mdl_mem + (a - BASE), 2); return v; }
static void mdl_wr(void *, uint32_t a, uint16_t v) { memcpy(mdl_mem + (a - BASE), &v, 2); }

static Vmrdp_top *top;
static uint64_t cycles;
static std::deque<uint32_t> cmdq;                           // words for the RTL
struct Rd { uint64_t due; uint32_t addr; };
static std::deque<Rd> reads;
static std::deque<uint32_t> wr_addrs;                        // accepted write commands
static unsigned rng = 12345;
static unsigned rnd() { rng = rng * 1103515245u + 12345u; return rng >> 16; }
static uint64_t n_rd, n_wr, n_wr_fb, n_wr_zb, n_wr_other;
static uint32_t other_first;

static void tick()
{
    // ---- drive inputs from the current state
    top->cmd_valid = !cmdq.empty();
    top->cmd_data = cmdq.empty() ? 0 : cmdq.front();
    top->m_cmd_ready = (rnd() % 8) != 0;
    top->m_wdata_ready = !wr_addrs.empty() && (rnd() % 8) != 0;
    bool rv = !reads.empty() && reads.front().due <= cycles;
    top->m_rdata_valid = rv;
    if (rv) { uint32_t v; memcpy(&v, rtl_mem + (reads.front().addr - BASE), 4); top->m_rdata = v; }
    top->clk = 0; top->eval();
    // ---- handshakes seen at this edge
    bool cmd_take = top->cmd_valid && top->cmd_ready;
    bool mc = top->m_cmd_valid && top->m_cmd_ready;
    uint32_t maddr = top->m_cmd_addr; bool mwe = top->m_cmd_we;
    bool wd = top->m_wdata_valid && top->m_wdata_ready;
    uint32_t wdat = top->m_wdata, wwe = top->m_wdata_we;
    top->clk = 1; top->eval();
    cycles++;
    if (cmd_take) cmdq.pop_front();
    if (rv) reads.pop_front();
    if (mc) {
        if (maddr < BASE || maddr >= BASE + SIZE) { fprintf(stderr, "RTL address out of range %08x\n", maddr); exit(2); }
        if (mwe) wr_addrs.push_back(maddr);
        else { reads.push_back({ cycles + 6 + rnd() % 12, maddr }); n_rd++; }
    }
    if (wd) {
        uint32_t a = wr_addrs.front(); wr_addrs.pop_front();
        for (int b = 0; b < 4; b++) if (wwe & (1u << b)) rtl_mem[a - BASE + b] = (uint8_t)(wdat >> (8 * b));
        n_wr++;
        if (a >= 0x40C00000u && a < 0x40C00000u + 268 * 240 * 2) n_wr_fb++;
        else if (a >= 0x40D01000u && a < 0x40D01000u + 268 * 240 * 2) n_wr_zb++;
        else { if (!n_wr_other) other_first = a; n_wr_other++; }
    }
}

static bool rtl_idle() { return cmdq.empty() && top->idle && reads.empty() && wr_addrs.empty(); }

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    if (argc < 2) { fprintf(stderr, "usage: tb_mrdp capture.bin [out.ppm]\n"); return 2; }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 2; }
    rtl_mem = (uint8_t *)calloc(SIZE, 1);
    mdl_mem = (uint8_t *)calloc(SIZE, 1);
    static mrdp_t mdl;
    mrdp_init(&mdl, nullptr, mdl_rd, mdl_wr);
    if (const char *e = getenv("TB_PIXEL")) sscanf(e, "%d,%d", &mdl.dbg_x, &mdl.dbg_y);  // needs -DMRDP_DEBUG_PIXEL

    top = new Vmrdp_top;
    top->rst = 1;
    for (int i = 0; i < 8; i++) tick();
    top->rst = 0;

    uint64_t nwords = 0, nmem = 0;
    uint32_t h[3];
    while (fread(h, 4, 1, fp) == 1) {
        if (h[0] == 1) {
            if (fread(&h[1], 4, 1, fp) != 1) break;
            std::vector<uint32_t> w(h[1]);
            if (fread(w.data(), 4, h[1], fp) != h[1]) break;
            static bool step = getenv("TB_STEP") != nullptr;
            static uint64_t ncmd;
            static std::vector<uint32_t> curcmd;
            for (uint32_t x : w) {
                cmdq.push_back(x); mrdp_push(&mdl, x);
                curcmd.push_back(x);
                if (mdl.ncmd != 0) continue;               // command not complete yet
                ncmd++;
                if (step) {
                    while (!rtl_idle()) tick();
                    for (int i = 0; i < 16; i++) tick();
                    if (memcmp(rtl_mem + (0x40C00000u - BASE), mdl_mem + (0x40C00000u - BASE), 0x200000)) {
                        uint32_t o = 0; while (!memcmp(rtl_mem + (0x40C00000u - BASE) + o, mdl_mem + (0x40C00000u - BASE) + o, 2)) o += 2;
                        uint32_t px = o / 2 % 268, py = o / 2 / 268;
                        printf("FIRST DIVERGENCE after command #%llu op %02x (%zu words) at x=%u y=%u rtl %04x model %04x\n",
                               (unsigned long long)ncmd, (curcmd[0] >> 24) & 0x3F, curcmd.size(), px, py,
                               *(uint16_t *)(rtl_mem + (0x40C00000u - BASE) + o), *(uint16_t *)(mdl_mem + (0x40C00000u - BASE) + o));
                        printf("  other %06x %08x  combine %06x %08x  prim %08x env %08x blend %08x fog %08x fill %08x\n",
                               mdl.other_h, mdl.other_l, mdl.combine_hi, mdl.combine_lo,
                               *(uint32_t *)&mdl.prim, *(uint32_t *)&mdl.env, *(uint32_t *)&mdl.blend, *(uint32_t *)&mdl.fog, mdl.fill);
                        for (size_t i = 0; i < curcmd.size(); i++) printf("  w%zu %08x\n", i, curcmd[i]);
                        for (int yy = 0; yy < 240; yy++) {      // the differing pixels, per row
                            int x0 = -1, x1 = -1, nd = 0;
                            for (int xx = 0; xx < 268; xx++) {
                                uint32_t oo = (0x40C00000u - BASE) + 2u * (yy * 268 + xx);
                                if (memcmp(rtl_mem + oo, mdl_mem + oo, 2)) { if (x0 < 0) x0 = xx; x1 = xx; nd++; }
                            }
                            if (nd) printf("  row %d: %d differ, x %d..%d\n", yy, nd, x0, x1);
                        }
                        exit(1);
                    }
                }
                curcmd.clear();
            }
            nwords += h[1];
            while (cmdq.size() > 64) tick();          // keep the RTL fed, not flooded
        } else if (h[0] == 2) {
            if (fread(&h[1], 4, 2, fp) != 2) break;
            std::vector<uint8_t> d(h[2]);
            if (fread(d.data(), 1, h[2], fp) != h[2]) break;
            // the geom core writes a staging slot only after the RDP used it
            while (!rtl_idle()) tick();
            memcpy(rtl_mem + (h[1] - BASE), d.data(), h[2]);
            memcpy(mdl_mem + (h[1] - BASE), d.data(), h[2]);
            nmem++;
        } else {
            fprintf(stderr, "bad event %u\n", h[0]);
            return 2;
        }
    }
    uint64_t guard = 0;
    while (!rtl_idle() && guard++ < 200000000ull) tick();
    for (int i = 0; i < 64; i++) tick();

    printf("events: %llu command words, %llu memory blocks; RTL %llu cycles, %llu reads, %llu writes\n",
           (unsigned long long)nwords, (unsigned long long)nmem, (unsigned long long)cycles,
           (unsigned long long)n_rd, (unsigned long long)n_wr);
    printf("RTL writes: fb %llu zb %llu other %llu (first %08x)\n", (unsigned long long)n_wr_fb,
           (unsigned long long)n_wr_zb, (unsigned long long)n_wr_other, other_first);
    printf("RTL: sync %u loads %u unknown %u | model: sync %u unknown %u pixels %llu\n",
           top->sync_count, top->load_count, top->unknown_ops, mdl.sync_count, mdl.unknown_ops,
           (unsigned long long)mdl.pixels_drawn);

    // ---- compare
    uint64_t diff = 0;
    uint32_t first = 0;
    for (uint32_t o = 0; o < SIZE; o += 2) {
        if (memcmp(rtl_mem + o, mdl_mem + o, 2)) { if (!diff) first = BASE + o; diff++; }
    }
    const uint32_t FB = 0x40C00000u, ZB = 0x40D01000u, W = 268, H = 240;
    auto region = [&](const char *name, uint32_t base) {
        uint64_t d = 0; int fx = -1, fy = -1; uint16_t a = 0, b = 0;
        for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) {
            uint32_t o = base - BASE + (y * W + x) * 2;
            uint16_t r, m; memcpy(&r, rtl_mem + o, 2); memcpy(&m, mdl_mem + o, 2);
            if (r != m) { if (!d) { fx = (int)x; fy = (int)y; a = r; b = m; } d++; }
        }
        printf("  %s: %llu pixels differ", name, (unsigned long long)d);
        if (d) printf(" (first at %d,%d: rtl %04x model %04x)", fx, fy, a, b);
        printf("\n");
    };
    region("color", FB);
    region("depth", ZB);
    if (argc > 2) {
        FILE *o = fopen(argv[2], "wb");
        fprintf(o, "P6\n%u %u\n255\n", W, H);
        for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) {
            uint16_t v; memcpy(&v, rtl_mem + (FB - BASE) + (y * W + x) * 2, 2);
            unsigned r5 = v >> 11, g6 = (v >> 5) & 63, b5 = v & 31;
            unsigned char c[3] = { (unsigned char)(r5 << 3 | r5 >> 2), (unsigned char)(g6 << 2 | g6 >> 4), (unsigned char)(b5 << 3 | b5 >> 2) };
            fwrite(c, 1, 3, o);
        }
        fclose(o);
    }
    if (diff) printf("MISMATCH: %llu halfwords differ, first at %08x\n", (unsigned long long)diff, first);
    else printf("RTL == model (all 64 MiB)\n");
    delete top;
    return diff != 0;
}
