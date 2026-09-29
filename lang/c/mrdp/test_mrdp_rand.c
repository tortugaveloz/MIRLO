/* Random MRDP command streams, for the RTL-vs-model differential test:
 *   test_mrdp_rand <seed> <capture>   then   sim/mrdp/obj_dir/tb_mrdp <capture>
 * Every mode the game does not use yet gets exercised: 2-cycle, copy and fill
 * modes, every combiner / blender mux, alpha compare and coverage, Z modes,
 * texture formats, wrap / mirror / clamp / shift, flipped texrects, fill
 * rectangles outside fill mode. Only the capture matters; the model is run
 * here too just to keep the texture memory events consistent.
 *   cc -O2 -o test_mrdp_rand test_mrdp_rand.c mrdp.c -lm */
#include "mrdp.h"
#include "mrdp_setup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defaults are the game's frame; the SoC-level test (litex/sim_mrdp.py)
 * shrinks everything to fit its firmware ROM:
 *   MRDP_RAND_N prims, MRDP_RAND_W/H frame, MRDP_RAND_FB/ZB/TEX addresses,
 *   MRDP_RAND_TEXMAX largest texture side as log2 (2..6) */
static unsigned W = 268, H = 240, NPRIM = 60, TEXMAX = 6;
static uint32_t FB = 0x40C00000u, ZB = 0x40D01000u, TEX = 0x41400000u;
static unsigned envu(const char *n, unsigned d) { const char *v = getenv(n); return v ? (unsigned)strtoul(v, 0, 0) : d; }

static FILE *g_cap;
static unsigned g_rng;
static unsigned rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }
static unsigned rr(unsigned n) { return rnd() % n; }

static void word(uint32_t w) { uint32_t h[3] = { 1, 1, w }; fwrite(h, 4, 3, g_cap); }
static void cmd(uint32_t hi, uint32_t lo) { word(hi); word(lo); }
static void mem(uint32_t addr, const void *p, uint32_t n)
{
    uint32_t h[3] = { 2, addr, n };
    fwrite(h, 4, 3, g_cap);
    fwrite(p, 1, n, g_cap);
}

static int32_t fx(double v) { return (int32_t)(v * 65536.0); }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: test_mrdp_rand seed capture\n"); return 2; }
    g_rng = (unsigned)strtoul(argv[1], 0, 0) * 2654435761u + 1;
    g_cap = fopen(argv[2], "wb");
    W = envu("MRDP_RAND_W", W); H = envu("MRDP_RAND_H", H); NPRIM = envu("MRDP_RAND_N", NPRIM);
    FB = envu("MRDP_RAND_FB", FB); ZB = envu("MRDP_RAND_ZB", ZB); TEX = envu("MRDP_RAND_TEX", TEX);
    TEXMAX = envu("MRDP_RAND_TEXMAX", TEXMAX);

    /* frame: clears */
    cmd(MRDP_OP_SET_SCISSOR << 24, ((W << 2) << 12) | (H << 2));
    cmd(MRDP_OP_SET_ZIMG << 24, ZB);
    cmd(MRDP_OP_SET_OTHER << 24 | (3u << MRDP_H_CYCLE_SHIFT), 0);
    cmd(MRDP_OP_SET_CIMG << 24 | (W - 1), ZB);
    cmd(MRDP_OP_SET_FILL << 24, 0xFFFFFFFF);
    cmd(MRDP_OP_FILL_RECT << 24 | (((W - 1) << 2) << 12) | ((H - 1) << 2), 0);
    cmd(MRDP_OP_SET_CIMG << 24 | (W - 1), FB);
    cmd(MRDP_OP_SET_FILL << 24, 0x39E739E7);
    cmd(MRDP_OP_FILL_RECT << 24 | (((W - 1) << 2) << 12) | ((H - 1) << 2), 0);

    /* textures: a few random ones, each loaded then drawn */
    for (unsigned prim = 0; prim < NPRIM; prim++) {
        /* a texture of random size and format */
        unsigned tw = 1u << (2 + rr(TEXMAX - 1)), th = 1u << (2 + rr(TEXMAX - 1));   /* 4..2^TEXMAX */
        if (rr(4) == 0) tw = 4 + rr((1u << TEXMAX) - 4);             /* not a power of two */
        if (tw * th > 4096) th = 4096 / tw;
        unsigned fmt = rr(3);
        static uint16_t tex[4096];
        for (unsigned i = 0; i < tw * th; i++) tex[i] = (uint16_t)rnd();
        mem(TEX, tex, (tw * th * 2 + 3) & ~3u);
        uint32_t line = (tw + 3) >> 2;
        cmd(MRDP_OP_SET_TIMG << 24 | (tw - 1), TEX);
        cmd(MRDP_OP_SET_TILE << 24 | (fmt << 21) | (line << 9), 7u << 24);
        cmd(MRDP_OP_LOAD_TILE << 24, (7u << 24) | (((tw - 1) << 2) << 12) | ((th - 1) << 2));
        unsigned ms = 0, mt = 0;
        while ((1u << ms) < tw) ms++;
        while ((1u << mt) < th) mt++;
        if ((1u << ms) != tw || rr(3) == 0) ms = 0;
        if ((1u << mt) != th || rr(3) == 0) mt = 0;
        unsigned tile = rr(8) == 0 ? 3 : 0;
        uint32_t t1 = (tile << 24) | (rr(2) << 19) | (rr(2) << 18) | (mt << 14) | ((rr(4) == 0 ? rr(16) : 0) << 10)
                    | (rr(2) << 9) | (rr(2) << 8) | (ms << 4) | (rr(4) == 0 ? rr(16) : 0);
        cmd(MRDP_OP_SET_TILE << 24 | (fmt << 21) | (line << 9), t1);
        cmd(MRDP_OP_SET_TILESIZE << 24 | ((rr(4) << 2) << 12) | (rr(4) << 2),
            (tile << 24) | (((tw - 1) << 2) << 12) | ((th - 1) << 2));

        /* random modes */
        unsigned cyc = rr(10) < 5 ? 0 : rr(10) < 7 ? 1 : rr(2) ? 2 : 3;
        uint32_t h = (cyc << MRDP_H_CYCLE_SHIFT) | (rr(2) ? MRDP_H_PERSP : 0) | ((rr(2) ? 2u : 0u) << MRDP_H_FILT_SHIFT);
        uint32_t l = (rnd() & 0xFFFF0000u) | (rr(2) ? MRDP_L_Z_CMP : 0) | (rr(2) ? MRDP_L_Z_UPD : 0)
                   | (rr(3) << MRDP_L_ZMODE_SHIFT) | (rr(6) == 0 ? MRDP_L_ALPHA_CMP : 0)
                   | (rr(6) == 0 ? MRDP_L_CVG_X_ALPHA : 0) | (rr(2) ? MRDP_L_FORCE_BL : 0)
                   | (rr(6) == 0 ? MRDP_L_Z_SRC_PRIM : 0) | (rr(2) ? MRDP_L_AA_EN : 0);
        if (rr(4) == 0) l |= 3u << MRDP_L_ZMODE_SHIFT;
        cmd(MRDP_OP_SET_OTHER << 24 | h, l);
        cmd(MRDP_OP_SET_COMBINE << 24 | (rnd() & 0xFFFFFF), rnd() ^ (rnd() << 16));
        cmd(MRDP_OP_SET_PRIM << 24 | rr(256), rnd() ^ (rnd() << 16));
        cmd(MRDP_OP_SET_ENV << 24, rnd() ^ (rnd() << 16));
        cmd(MRDP_OP_SET_FOG << 24, rnd() ^ (rnd() << 16));
        cmd(MRDP_OP_SET_BLEND << 24, rnd() ^ (rnd() << 16));
        cmd(MRDP_OP_SET_PRIM_Z << 24, (rnd() & 0xFFFF) << 16);
        cmd(MRDP_OP_SET_FILL << 24, rnd() ^ (rnd() << 16));

        unsigned kind = rr(10);
        if (kind < 6) {
            /* triangle with random attributes */
            mrdp_vtx_t v[3];
            for (int k = 0; k < 3; k++) {
                v[k].x = fx(-20 + rr(W + 40) + rr(65536) / 65536.0);
                v[k].y = fx(-20 + rr(H + 40) + rr(65536) / 65536.0);
                for (int c = 0; c < 4; c++) v[k].a[c] = fx(rr(300) - 20);
                double w = 0.1 + rr(1000) / 1000.0;
                v[k].a[6] = fx(w);
                v[k].a[4] = fx((rr(400) - 200) * w);
                v[k].a[5] = fx((rr(400) - 200) * w);
                v[k].a[7] = (int32_t)(rr(0x7FFF) << 16);
            }
            uint32_t w[44];
            /* the forms MRDP's RTL takes: TRI_V (TRI_G for slivers), TRI_G
             * always, TRI_V with arbitrary factors (its saturations) */
            unsigned form = rr(6), fl = rr(8);
            int n;
            if (form >= 3) {
                /* TRI_R from raw fields: colour 0..1, s t texels, w, z 0..1 */
                for (int k = 0; k < 3; k++) {
                    for (int c = 0; c < 4; c++) v[k].a[c] = (int32_t)rr(0x10001);
                    v[k].a[6] = fx(0.2 + rr(4000) / 1000.0);
                    v[k].a[4] = fx(rr(400) - 200.0); v[k].a[5] = fx(rr(400) - 200.0);
                    v[k].a[7] = (int32_t)rr(0x10001);
                }
                n = mrdp_setup_triangle_r(w, &v[0], &v[1], &v[2], fl, tile, form == 3 && rr(4) == 0);
                if (n && form == 5 && (w[0] >> 24) == MRDP_OP_TRI_R)
                    for (int k = 1; k < n; k++) if (rr(4) == 0) w[k] = rnd() ^ (rnd() << 16);   /* anything */
            } else
                n = form == 0 ? mrdp_setup_triangle_g(w, &v[0], &v[1], &v[2], fl, tile, (int)rr(2))
                              : mrdp_setup_triangle_v(w, &v[0], &v[1], &v[2], fl, tile, (int)rr(2));
            if (n && form == 2 && ((w[0] >> 27) & 7u) == 2u) {
                /* TRI_V with arbitrary factors and offsets: MRDP's saturations */
                for (int k = 8; k < 14; k++) if (rr(2)) w[k] = rnd() ^ (rnd() << 16);
            }
            for (int i = 0; i < n; i++) word(w[i]);
        } else if (kind < 9) {
            /* texrect, sometimes flipped */
            unsigned x0 = rr(W), y0 = rr(H), x1 = x0 + rr(80), y1 = y0 + rr(80);
            unsigned op = rr(3) ? MRDP_OP_TEXRECT : MRDP_OP_TEXRECT_FLIP;
            cmd(op << 24 | ((x1 << 2) << 12) | (y1 << 2), (tile << 24) | ((x0 << 2) << 12) | (y0 << 2));
            cmd(((rnd() & 0xFFFF) << 16) | (rnd() & 0xFFFF), ((rr(4096) - 2048) & 0xFFFF) << 16 | ((rr(4096) - 2048) & 0xFFFF));
        } else {
            unsigned x0 = rr(W), y0 = rr(H), x1 = x0 + rr(60), y1 = y0 + rr(60);
            cmd(MRDP_OP_FILL_RECT << 24 | ((x1 << 2) << 12) | (y1 << 2), ((x0 << 2) << 12) | (y0 << 2));
        }
    }
    cmd(MRDP_OP_SYNC_FULL << 24, 0);
    fclose(g_cap);
    return 0;
}
