// MIRLO's MIPS SoC (rtl/soc/mirlo_mips.sv) under Verilator: the game CPU,
// the geom core, the audio core, MRDP, the scan-out, the SDRAM controller +
// PHY -- all RTL -- against the SDR chip model of tb_sdr.cpp (with its
// protocol checks). The program is put where the Pocket loads data slot 0
// (SDRAM 0x4000_0000); the Pocket's reset exit comes LOAD_AT sys cycles in
// (NOLOAD=1: with nothing loaded -- the boot ROM's serial-boot path).
//
//   tb_soc prog.bin [-c Mcycles] [-o dir] [-e every_n_frames]
// The UART's bytes go to stdout; a 0x04 byte ends the run. Every n-th
// frame goes to dir/fNNNNNN.ppm (read back from the chip model's memory at
// the scan-out's base).
#include "Vsoc_sim_top.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <ctime>

static Vsoc_sim_top *t;
static long errors;
#define ERR(...) do { if (errors++ < 20) { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)

// ---- the chip (sys2x cycles): 4 banks x 8192 rows x 1024 columns x 16 bits
static std::vector<uint16_t> mem(1u << 25);
static int rows[4], opened[4];
static long t_act[4], t_pre[4], t_wr[4], t_ref = -1000;
static long cyc;
static uint16_t pend_v[8]; static long pend_c[8];      // read beats due (a small ring)
static long wburst = -1;
static uint32_t key(int ba, int row, int col) { return (uint32_t)ba << 23 | (uint32_t)row << 10 | (col & 0x3ff); }
// the controller's word address -> the halves' keys (sdr_ctrl: row a[23:11], bank a[10:9], column pair a[8:0])
static uint32_t wkey(uint32_t w, int half) { return key((w >> 9) & 3, (w >> 11) & 0x1fff, ((w & 0x1ff) << 1) | half); }
static uint32_t rd_word(uint32_t w) { return mem[wkey(w, 0)] | (uint32_t)mem[wkey(w, 1)] << 16; }
static void wr_word(uint32_t w, uint32_t v) { mem[wkey(w, 0)] = v & 0xffff; mem[wkey(w, 1)] = v >> 16; }

static void store(uint32_t k, uint16_t dq, int dm)
{
    uint16_t v = mem[k];
    if (!(dm & 1)) v = (v & 0xff00) | (dq & 0x00ff);
    if (!(dm & 2)) v = (v & 0x00ff) | (dq & 0xff00);
    mem[k] = v;
}
static void chip_step()
{
    int ba = t->sdram_ba, a = t->sdram_a, dm = t->sdram_dm;
    uint16_t dq = t->dq_out;
    bool oe = t->dq_oe;
    if (wburst >= 0) { store((uint32_t)wburst, dq, dm); if (!oe) ERR("cyc %ld: write beat 2 without DQ", cyc); wburst = -1; }
    for (int i = 0; i < 8; i++) if (pend_c[i] == cyc && oe) ERR("cyc %ld: DQ contention", cyc);
    if (!t->sdram_cke) return;
    int c = t->sdram_ras_n << 2 | t->sdram_cas_n << 1 | t->sdram_we_n;
    if (c != 7 && cyc - t_ref < 11) ERR("cyc %ld: command %d inside tRFC", cyc, c);
    switch (c) {
    case 3:
        if (opened[ba]) ERR("cyc %ld: ACT to open bank %d", cyc, ba);
        if (cyc - t_pre[ba] < 3) ERR("cyc %ld: tRP", cyc);
        opened[ba] = 1; rows[ba] = a; t_act[ba] = cyc; break;
    case 2:
        for (int b = 0; b < 4; b++) if ((a & 0x400) || b == ba) {
            if (opened[b] && cyc - t_act[b] < 7) ERR("cyc %ld: tRAS", cyc);
            if (opened[b] && cyc - t_wr[b] < 3) ERR("cyc %ld: tWR", cyc);
            opened[b] = 0; t_pre[b] = cyc;
        }
        break;
    case 4: {
        if (!opened[ba]) ERR("cyc %ld: WRITE to closed bank", cyc);
        if (cyc - t_act[ba] < 3) ERR("cyc %ld: tRCD (write)", cyc);
        int col = a & 0x3ff;
        store(key(ba, rows[ba], col), dq, dm);
        wburst = key(ba, rows[ba], col ^ 1);
        t_wr[ba] = cyc + 1;
        break;
    }
    case 5: {
        if (!opened[ba]) ERR("cyc %ld: READ from closed bank", cyc);
        if (cyc - t_act[ba] < 3) ERR("cyc %ld: tRCD (read)", cyc);
        int col = a & 0x3ff;
        for (int k = 0; k < 2; k++) {
            int s = (int)((cyc + 3 + k) & 7);
            pend_c[s] = cyc + 3 + k; pend_v[s] = mem[key(ba, rows[ba], col ^ k)];
        }
        break;
    }
    case 1:
        for (int b = 0; b < 4; b++) if (opened[b]) ERR("cyc %ld: REF with a bank open", cyc);
        t_ref = cyc; break;
    default: break;
    }
}

static void dump_frame(const std::string &dir, long n)
{
    char path[512];
    snprintf(path, sizeof path, "%s/f%06ld.ppm", dir.c_str(), n);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int w = t->fb_mode ? 320 : 268;
    fprintf(f, "P6 %d 240 255\n", w);
    uint32_t w0 = (t->fb_base & 0x3FFFFFF) >> 2;
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < w; x += 2) {
            uint32_t v = rd_word(w0 + (y * w + x) / 2);
            for (int h = 0; h < 2; h++) {
                uint16_t p = h ? v >> 16 : v & 0xffff;
                uint8_t rgb[3] = { (uint8_t)((p >> 11) << 3), (uint8_t)(((p >> 5) & 63) << 2), (uint8_t)((p & 31) << 3) };
                fwrite(rgb, 1, 3, f);
            }
        }
    fclose(f);
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    if (argc < 2) { fprintf(stderr, "usage: tb_soc prog.bin [-c Mcycles] [-o dir] [-e every]\n"); return 2; }
    long mcycles = 30, every = 1;
    std::string outdir;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) mcycles = atol(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "-e") && i + 1 < argc) every = atol(argv[++i]);
    }
    // the program, where the Pocket puts slot 0: SDRAM 0x4000_0000, little-endian words
    FILE *rf = fopen(argv[1], "rb");
    if (!rf) { perror(argv[1]); return 2; }
    std::vector<uint8_t> img;
    { uint8_t b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, rf)) > 0) img.insert(img.end(), b, b + n); }
    fclose(rf);
    img.resize((img.size() + 3) & ~3u);
    bool noload = getenv("NOLOAD") != nullptr;
    if (!noload)
        for (size_t i = 0; i < img.size(); i += 4)
            wr_word((uint32_t)(i / 4), (uint32_t)img[i] | img[i + 1] << 8 | img[i + 2] << 16 | (uint32_t)img[i + 3] << 24);
    printf("program: %zu bytes at 0x40000000%s\n", img.size(), noload ? " (NOLOAD: not given)" : "");

    t = new Vsoc_sim_top;
    long load_at = getenv("LOAD_AT") ? atol(getenv("LOAD_AT")) : 3000;
    t->host_reset_n = 0; t->host_loaded = 0; t->cont1_key = 0;
    t->file_size = noload ? 0u : (uint32_t)img.size(); t->br_complete = 0;
    long br_done_at = -1;                           // the bridge's read completes then (sys cycles)
    t->reset = 1;
    for (int i = 0; i < 8; i++) pend_c[i] = -1;
    for (int b = 0; b < 4; b++) t_act[b] = t_pre[b] = t_wr[b] = -1000;
    long sys = 0, frames = 0;
    const long max_sys = mcycles * 1000000;
    time_t t0 = time(nullptr);
    int vid_phase = 0;                              // clk_vid: sys / 11
    int pvb = 0;
    long audio_words = 0;
    bool done = false;
    for (cyc = 0; sys < max_sys && !done; cyc++) {
        bool sys_edge = (cyc & 1) == 0;
        t->clk_sys2x = 1;
        if (sys_edge) {
            t->clk_sys = 1;
            if (sys == 20) t->reset = 0;
            if (sys == load_at) { t->host_reset_n = 1; t->host_loaded = !noload; }
            if (++vid_phase == 11) vid_phase = 0;
            t->clk_vid = vid_phase < 6;
        }
        t->eval();
        chip_step();
        uint16_t v = 0;
        for (int i = 0; i < 8; i++) if (pend_c[i] == cyc) { v = pend_v[i]; pend_c[i] = -1; }
        t->dq_in = v;
        t->eval();
        t->clk_sys2x = 0; if (!sys_edge) t->clk_sys = 0;
        t->eval();
        if (!sys_edge) {
            static long ut = getenv("UTRACE") ? atol(getenv("UTRACE")) : 0;
            if (ut && t->uart_we) { ut--; printf("[u %ld pc %08x xreq %d xaddr %08x byte %02x]\n", sys, t->cpu_pc, t->x_req, t->x_addr, t->uart_byte); }
            if (t->uart_we) {
                if (t->uart_byte == 4) done = true;
                else { putchar(t->uart_byte); if (t->uart_byte == '\n') fflush(stdout); }
            }
            if (t->audio_wr) audio_words++;
            // the Pocket's bridge: slot 0 is the program's file; a read lands in SDRAM
            // at ~40 MB/s (the APF bridge's rate), then the completion toggles high
            if (t->br_req_read) {
                uint32_t off = t->br_offset, len = t->br_length, a = t->br_addr;
                if (t->br_slot == 0 && off + len <= img.size() && (a & 3) == 0 && (len & 3) == 0)
                    for (uint32_t i = 0; i < len; i += 4)
                        wr_word(((a & 0x3FFFFFF) >> 2) + i / 4, (uint32_t)img[off + i] | img[off + i + 1] << 8 |
                                img[off + i + 2] << 16 | (uint32_t)img[off + i + 3] << 24);
                else printf("[bridge: read slot %u off %u len %u -> %08x not modelled]\n", t->br_slot, off, len, a);
                t->br_complete = 0;
                br_done_at = sys + 200 + len * 62832000.0 / 40e6;
            }
            if (br_done_at >= 0 && sys >= br_done_at) { t->br_complete = 1; br_done_at = -1; }
            static long xt = getenv("XTRACE") ? atol(getenv("XTRACE")) : 0;
            if (xt && t->x_ack) { xt--; printf("[x %s %08x = %08x]\n", t->x_we ? "W" : "R", t->x_addr, t->x_we ? t->x_wdata : t->x_rdata); }
            if (t->vblank && !pvb) {
                if (!outdir.empty() && frames % every == 0) dump_frame(outdir, frames);
                frames++;
            }
            pvb = t->vblank;
            long ev = getenv("EVERY") ? atol(getenv("EVERY")) : 0;
            if (ev && sys % ev == 0) {
                printf("[%ld M cycles, %lds] frame %ld cpu pc %08x geom pc %08x hb %08x audio pc %08x st %08x words %ld mrdp syncs %u\n",
                       sys / 1000000, (long)(time(nullptr) - t0), frames, t->cpu_pc, t->geom_pc, t->geom_msg,
                       t->audio_pc, t->audio_state, audio_words, t->mrdp_sync);
                fflush(stdout);
            }
            sys++;
        }
    }
    fflush(stdout);
    printf("\n%ld sys cycles, %ld frames, %ld SDRAM protocol errors; cpu pc %08x cause %08x epc %08x\n",
           sys, frames, errors, t->cpu_pc, t->cpu_cause, t->cpu_epc);
    if (const char *d = getenv("DUMP")) {               // DUMP=addr,len: SDRAM words out (hex)
        uint32_t a = strtoul(d, 0, 0), n = strchr(d, ',') ? strtoul(strchr(d, ',') + 1, 0, 0) : 16;
        for (uint32_t i = 0; i < n; i++) printf("%08x: %08x\n", a + 4 * i, rd_word(((a & 0x3FFFFFF) >> 2) + i));
    }
    delete t;
    return errors ? 1 : 0;
}
