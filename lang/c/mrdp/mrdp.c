/* MRDP bit-exact reference model -- see mrdp.h (the contract) and
 * docs/mrdp.md (the design). Integer arithmetic only; every shift, round and
 * clamp here is what the RTL does. */
#include "mrdp.h"
#include "mrdp_setup.h"

#include <string.h>
#if defined(MRDP_DEBUG_PIXEL) || defined(MRDP_DEBUG_AA)
#include <stdio.h>
#endif

/* ---- helpers ------------------------------------------------------------ */

static inline int32_t sext(uint32_t v, int bits)
{
    return (int32_t)(v << (32 - bits)) >> (32 - bits);
}
static inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

int mrdp_cmd_len(uint32_t w0)
{
    unsigned op = (w0 >> 24) & 0x3F;
    if (op == MRDP_OP_TRI_R)
        return (int)(mrdp_tri_r_words((w0 >> 19) & 7u) / 2u);
    if (op >= 0x08 && op <= 0x0F)
        return 4 + ((op & 4) ? 8 : 0) + ((op & 2) ? 8 : 0) + ((op & 1) ? 2 : 0);
    if (op >= MRDP_OP_TRI_V && op <= (MRDP_OP_TRI_V | 7u))
        return (int)(mrdp_tri_v_words(op & 7u) / 2u);
    if (op >= MRDP_OP_TRI_G && op <= (MRDP_OP_TRI_G | 7u))
        return (int)(mrdp_tri_g_words(op & 7u) / 2u);
    switch (op) {
    case MRDP_OP_TEXRECT: case MRDP_OP_TEXRECT_FLIP:
        return 2;
    case MRDP_OP_NOP: case MRDP_OP_SYNC_LOAD: case MRDP_OP_SYNC_PIPE:
    case MRDP_OP_SYNC_TILE: case MRDP_OP_SYNC_FULL: case MRDP_OP_SET_SCISSOR:
    case MRDP_OP_SET_PRIM_Z: case MRDP_OP_SET_OTHER: case MRDP_OP_SET_TILESIZE:
    case MRDP_OP_LOAD_TILE: case MRDP_OP_SET_TILE: case MRDP_OP_FILL_RECT:
    case MRDP_OP_SET_FILL: case MRDP_OP_SET_FOG: case MRDP_OP_SET_BLEND:
    case MRDP_OP_SET_PRIM: case MRDP_OP_SET_ENV: case MRDP_OP_SET_COMBINE:
    case MRDP_OP_SET_TIMG: case MRDP_OP_SET_ZIMG: case MRDP_OP_SET_CIMG:
        return 1;
    default:
        return 1;   /* unknown: one word, counted and skipped */
    }
}

/* ---- perspective: 1/W from a 64-entry table + linear step ---------------
 * W > 0 is normalised so bit 30 is its top bit (wn = W << sh). With
 * idx = wn[29:24] and frac = wn[23:15]:
 *   r = P[idx] - ((D[idx] * frac) >> 9),  P[i] = round(2^22 / (64 + i))
 * so r ~ 2^16 / m for the mantissa m = wn / 2^30 in [1, 2), r in [2^15, 2^16].
 * Then S/W in s10.5 is (S * r) >> (27 - sh), W being 2.30 (1.0 = 2^30). */
static uint32_t s_rcp_p[65], s_rcp_d[64];
static int s_rcp_ready;

static void rcp_init(void)
{
    for (int i = 0; i <= 64; i++)
        s_rcp_p[i] = ((1u << 23) / (64 + i) + 1) >> 1;   /* round(2^22/(64+i)) */
    for (int i = 0; i < 64; i++)
        s_rcp_d[i] = s_rcp_p[i] - s_rcp_p[i + 1];
    s_rcp_ready = 1;
}

uint32_t mrdp_rcp(uint32_t w, int *shift)
{
    if (!s_rcp_ready) rcp_init();
    if ((int32_t)w <= 0) w = 1;
    int sh = __builtin_clz(w) - 1;
    uint32_t wn = w << sh;
    uint32_t idx = (wn >> 24) & 63, frac = (wn >> 15) & 511;
    *shift = sh;
    return s_rcp_p[idx] - ((s_rcp_d[idx] * frac) >> 9);
}

int32_t mrdp_persp(int32_t s, uint32_t w)
{
    int sh;
    uint32_t r = mrdp_rcp(w, &sh);
    if (s > (1 << 26) - 1) s = (1 << 26) - 1;       /* the 27-bit multiplier input */
    if (s < -(1 << 26)) s = -(1 << 26);
    int64_t p = (int64_t)s * (int64_t)r;
    int k = 27 - sh;                  /* W is 2.30 (mrdp_slope30): 41 - 14 */
    int64_t v = p >> (k > 0 ? k : 0); /* saturated as the RTL's 45-bit product */
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int32_t)v;
}

static inline int32_t s105_noperp(int32_t s)
{
    int32_t v = s >> 11;
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

/* ---- init / state ------------------------------------------------------- */

void mrdp_init(mrdp_t *r, void *ctx, mrdp_rd16_t rd16, mrdp_wr16_t wr16)
{
    memset(r, 0, sizeof *r);
    r->ctx = ctx;
    r->rd16 = rd16;
    r->wr16 = wr16;
    r->cimg_width = 1;
    r->timg_width = 1;
    r->sc_xl = 1023 << 2;
    r->sc_yl = 1023 << 2;
    r->dbg_x = r->dbg_y = -1;
    if (!s_rcp_ready) rcp_init();
}

static inline mrdp_rgba_t rgba_of(uint32_t w)
{
    mrdp_rgba_t c = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w };
    return c;
}

/* ---- TMEM --------------------------------------------------------------- */

static inline void tmem_locate(const mrdp_tile_t *t, int s, int tt, int *bank, int *addr)
{
    *bank = ((tt & 1) << 1) | (s & 1);
    *addr = (t->tmem + (tt >> 1) * t->line * 2 + (s >> 1)) & 1023;
}

static mrdp_rgba_t texel_expand(unsigned fmt, uint16_t v)
{
    mrdp_rgba_t c;
    switch (fmt) {
    case 1:     /* IA88 */
        c.r = c.g = c.b = (uint8_t)(v >> 8);
        c.a = (uint8_t)v;
        break;
    case 2: {   /* RGBA4444 */
        c.r = (uint8_t)(((v >> 12) & 15) * 17);
        c.g = (uint8_t)(((v >> 8) & 15) * 17);
        c.b = (uint8_t)(((v >> 4) & 15) * 17);
        c.a = (uint8_t)((v & 15) * 17);
        break;
    }
    default: {  /* RGBA5551 */
        unsigned r5 = (v >> 11) & 31, g5 = (v >> 6) & 31, b5 = (v >> 1) & 31;
        c.r = (uint8_t)(r5 << 3 | r5 >> 2);
        c.g = (uint8_t)(g5 << 3 | g5 >> 2);
        c.b = (uint8_t)(b5 << 3 | b5 >> 2);
        c.a = (v & 1) ? 255 : 0;
        break;
    }
    }
    return c;
}

static mrdp_rgba_t tmem_fetch(const mrdp_t *r, const mrdp_tile_t *t, int s, int tt)
{
    int bank, addr;
    tmem_locate(t, s, tt, &bank, &addr);
    return texel_expand(t->fmt, r->tmem[bank][addr]);
}

/* ---- texture coordinate: shift, tile origin, clamp, mirror, mask ------ */

static int32_t tc_shift(int32_t v, unsigned shift)
{
    if (shift == 0) return v;
    if (shift <= 10) return v >> shift;
    return v << (16 - shift);
}

/* one axis: v in s10.5 after shift; returns the two integer texel
 * coordinates (v0, v0+1 after wrap) and the 5-bit fraction */
static void tc_axis(int32_t v, unsigned lo, unsigned hi, unsigned clampbit,
                    unsigned mirror, unsigned mask, int *c0, int *c1, int *frac)
{
    v -= (int32_t)lo << 3;                        /* u10.2 -> s10.5 */
    int i = v >> 5, f = v & 31;
    int j = i + 1;
    if (clampbit || mask == 0) {
        int mx = ((int)hi - (int)lo) >> 2;
        if (i < 0) { i = 0; j = 0; f = 0; }
        else if (i >= mx) { i = mx; j = mx; f = 0; }
    }
    if (mask) {
        int m = (1 << mask) - 1;
        if (mirror && ((i >> mask) & 1)) i = ~i;
        if (mirror && ((j >> mask) & 1)) j = ~j;
        i &= m;
        j &= m;
    }
    *c0 = i;
    *c1 = j;
    *frac = f;
}

static mrdp_rgba_t texture_sample(const mrdp_t *r, int tilei, int32_t s105, int32_t t105)
{
    const mrdp_tile_t *t = &r->tile[tilei & 7];
    int32_t s = tc_shift(s105, t->shifts), tt = tc_shift(t105, t->shiftt);
    int s0, s1, t0, t1, fs, ft;
    tc_axis(s, t->uls, t->lrs, t->cs, t->ms, t->masks, &s0, &s1, &fs);
    tc_axis(tt, t->ult, t->lrt, t->ct, t->mt, t->maskt, &t0, &t1, &ft);

    mrdp_rgba_t a = tmem_fetch(r, t, s0, t0);
    if (((r->other_h >> MRDP_H_FILT_SHIFT) & 3) == 0)   /* point */
        return a;

    /* N64 3-point filter: the triangle of the 2x2 the sample falls in */
    mrdp_rgba_t b = tmem_fetch(r, t, s1, t0);
    mrdp_rgba_t c = tmem_fetch(r, t, s0, t1);
    mrdp_rgba_t d = tmem_fetch(r, t, s1, t1);
    mrdp_rgba_t o;
    const uint8_t *pa = &a.r, *pb = &b.r, *pc = &c.r, *pd = &d.r;
    uint8_t *po = &o.r;
    int upper = fs + ft < 32;
    for (int k = 0; k < 4; k++) {
        int v;
        if (upper)
            v = pa[k] + (((pb[k] - pa[k]) * fs + (pc[k] - pa[k]) * ft + 16) >> 5);
        else
            v = pd[k] + (((pc[k] - pd[k]) * (32 - fs) + (pb[k] - pd[k]) * (32 - ft) + 16) >> 5);
        po[k] = (uint8_t)clamp8(v);
    }
    return o;
}

/* ---- color combiner: (A - B) * C + D, N64 selections -------------------- */

typedef struct {
    mrdp_rgba_t tex, shade, combined;
} cc_in_t;

static int cc_rgb_a(const mrdp_t *r, const cc_in_t *in, unsigned sel, int ch)
{
    const uint8_t *tex = &in->tex.r, *sh = &in->shade.r, *cm = &in->combined.r;
    const uint8_t *pr = &r->prim.r, *en = &r->env.r;
    switch (sel) {
    case 0: return cm[ch];
    case 1: case 2: return tex[ch];
    case 3: return pr[ch];
    case 4: return sh[ch];
    case 5: return en[ch];
    case 6: return 255;
    default: return 0;           /* 7 noise, 8+ zero */
    }
}
static int cc_rgb_b(const mrdp_t *r, const cc_in_t *in, unsigned sel, int ch)
{
    if (sel >= 6) return 0;       /* 6 key center, 7 K4, 8+ zero */
    return cc_rgb_a(r, in, sel, ch);
}
static int cc_rgb_c(const mrdp_t *r, const cc_in_t *in, unsigned sel, int ch)
{
    switch (sel) {
    case 0: case 1: case 2: case 3: case 4: case 5:
        return cc_rgb_a(r, in, sel, ch);
    case 7: return in->combined.a;
    case 8: case 9: return in->tex.a;
    case 10: return r->prim.a;
    case 11: return in->shade.a;
    case 12: return r->env.a;
    case 14: return r->prim_lod_frac;
    default: return 0;           /* 6 key scale, 13 LOD fraction, 15 K5, 16+ */
    }
}
static int cc_rgb_d(const mrdp_t *r, const cc_in_t *in, unsigned sel, int ch)
{
    return sel == 7 ? 0 : cc_rgb_a(r, in, sel, ch);   /* 6 = 1 */
}
static int cc_alpha_abd(const mrdp_t *r, const cc_in_t *in, unsigned sel)
{
    switch (sel) {
    case 0: return in->combined.a;
    case 1: case 2: return in->tex.a;
    case 3: return r->prim.a;
    case 4: return in->shade.a;
    case 5: return r->env.a;
    case 6: return 255;
    default: return 0;
    }
}
static int cc_alpha_c(const mrdp_t *r, const cc_in_t *in, unsigned sel)
{
    switch (sel) {
    case 1: case 2: return in->tex.a;
    case 3: return r->prim.a;
    case 4: return in->shade.a;
    case 5: return r->env.a;
    case 6: return r->prim_lod_frac;
    default: return 0;           /* 0 LOD fraction, 7 zero */
    }
}

/* (a - b) * c + d with c = 255 meaning 1.0 */
static inline int cc_eval(int a, int b, int c, int d)
{
    int cw = c + (c >> 7);
    return clamp8((((a - b) * cw) >> 8) + d);
}

static mrdp_rgba_t combine(const mrdp_t *r, const cc_in_t *in, int cyc)
{
    uint32_t hi = r->combine_hi, lo = r->combine_lo;
    unsigned sa_r, mul_r, sa_a, mul_a, sb_r, sb_a, add_r, add_a;
    if (cyc == 0) {
        sa_r = (hi >> 20) & 15; mul_r = (hi >> 15) & 31;
        sa_a = (hi >> 12) & 7;  mul_a = (hi >> 9) & 7;
        sb_r = (lo >> 28) & 15; add_r = (lo >> 15) & 7;
        sb_a = (lo >> 12) & 7;  add_a = (lo >> 9) & 7;
    } else {
        sa_r = (hi >> 5) & 15;  mul_r = hi & 31;
        sa_a = (lo >> 21) & 7;  mul_a = (lo >> 18) & 7;
        sb_r = (lo >> 24) & 15; add_r = (lo >> 6) & 7;
        sb_a = (lo >> 3) & 7;   add_a = lo & 7;
    }
    mrdp_rgba_t o;
    uint8_t *po = &o.r;
    for (int ch = 0; ch < 3; ch++)
        po[ch] = (uint8_t)cc_eval(cc_rgb_a(r, in, sa_r, ch), cc_rgb_b(r, in, sb_r, ch),
                                  cc_rgb_c(r, in, mul_r, ch), cc_rgb_d(r, in, add_r, ch));
    o.a = (uint8_t)cc_eval(cc_alpha_abd(r, in, sa_a), cc_alpha_abd(r, in, sb_a),
                           cc_alpha_c(r, in, mul_a), cc_alpha_abd(r, in, add_a));
    return o;
}

/* ---- blender: (P * a + M * b) >> 8 ------------------------------------- */

static mrdp_rgba_t bl_pm(const mrdp_t *r, unsigned sel, mrdp_rgba_t pix, mrdp_rgba_t mem)
{
    switch (sel) {
    case 0: return pix;
    case 1: return mem;
    case 2: return r->blend;
    default: return r->fog;
    }
}

static mrdp_rgba_t blend(const mrdp_t *r, int cyc, mrdp_rgba_t pix, mrdp_rgba_t mem,
                         int comb_a, int shade_a)
{
    uint32_t l = r->other_l;
    unsigned p = (l >> (cyc ? 28 : 30)) & 3, a = (l >> (cyc ? 24 : 26)) & 3;
    unsigned m = (l >> (cyc ? 20 : 22)) & 3, b = (l >> (cyc ? 16 : 18)) & 3;
    int av;
    switch (a) {
    case 0: av = comb_a; break;
    case 1: av = r->fog.a; break;
    case 2: av = shade_a; break;
    default: av = 0; break;
    }
    int aw = av + (av >> 7), bw;
    switch (b) {
    case 0: bw = 256 - aw; break;
    case 1: case 2: bw = 256; break;     /* memory alpha is 1: RGB565 has none */
    default: bw = 0; break;
    }
    mrdp_rgba_t P = bl_pm(r, p, pix, mem), M = bl_pm(r, m, pix, mem), o;
    o.r = (uint8_t)clamp8((P.r * aw + M.r * bw) >> 8);
    o.g = (uint8_t)clamp8((P.g * aw + M.g * bw) >> 8);
    o.b = (uint8_t)clamp8((P.b * aw + M.b * bw) >> 8);
    o.a = pix.a;
    return o;
}

static inline int bl_reads_memory(uint32_t l, int cyc)
{
    return ((l >> (cyc ? 28 : 30)) & 3) == 1 || ((l >> (cyc ? 20 : 22)) & 3) == 1;
}

/* ---- pixels -------------------------------------------------------------- */

static inline uint16_t rgb565(mrdp_rgba_t c)
{
    return (uint16_t)(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
}
static inline mrdp_rgba_t unrgb565(uint16_t v)
{
    unsigned r5 = v >> 11, g6 = (v >> 5) & 63, b5 = v & 31;
    mrdp_rgba_t c = { (uint8_t)(r5 << 3 | r5 >> 2), (uint8_t)(g6 << 2 | g6 >> 4),
                      (uint8_t)(b5 << 3 | b5 >> 2), 255 };
    return c;
}

typedef struct {
    int shade, tex, zbuf;          /* which attributes the primitive has */
    int tile;
    int texrect;                   /* s,t already in s10.5 */
    unsigned dz;                   /* AA_EN: the depth range of "the same surface" (draw_triangle_aa) */
} prim_t;

/* attribute order in the iterators */
enum { AR, AG, AB, AA, AS, AT, AW, AZ, NATTR };

static void draw_pixel(mrdp_t *r, const prim_t *pr, int x, int y, const int32_t *at, int cvgn)
{
    uint32_t h = r->other_h, l = r->other_l;
    unsigned cyc = (h >> MRDP_H_CYCLE_SHIFT) & 3;
    uint32_t pix_off = ((uint32_t)y * r->cimg_width + (uint32_t)x) * 2;
    uint32_t caddr = r->cimg + pix_off, zaddr = r->zimg + pix_off;

    if (cyc == 3) {                                /* fill mode */
        r->wr16(r->ctx, caddr, (x & 1) ? (uint16_t)r->fill : (uint16_t)(r->fill >> 16));
        r->pixels_drawn++;
        return;
    }

    /* depth */
    uint16_t depth;
    if ((l & MRDP_L_Z_SRC_PRIM) || !pr->zbuf) {
        depth = r->prim_z;
    } else {
        int32_t z = at[AZ] >> 15;
        depth = (uint16_t)(z < 0 ? 0 : z > 0xFFFF ? 0xFFFF : z);
    }
    if (l & MRDP_L_Z_CMP) {
        uint16_t old = r->rd16(r->ctx, zaddr);
        unsigned zmode = (l >> MRDP_L_ZMODE_SHIFT) & 3;
        int pass = zmode == 3 ? depth <= old : depth < old;
        /* AA_EN, a partly covered pixel on a surface within dz of the one
         * already there (an edge the two share): the one covering more of the
         * pixel keeps it, whichever is nearer -- the N64 blends the two by
         * coverage here, from coverage it stores in the framebuffer; without
         * that store the bridge carpet's 1/8-covered edge pixels won depth
         * ties on the wall below it (castle, 2026-09-28) */
        if (zmode != 3 && cvgn < 8) {
            int d = (int)depth - (int)old;
            if ((unsigned)(d < 0 ? -d : d) <= pr->dz) pass = cvgn >= 4;
        }
        if (!pass) return;
    }

    /* inputs */
    cc_in_t in;
    memset(&in, 0, sizeof in);
    if (pr->shade) {
        in.shade.r = (uint8_t)clamp8(at[AR] >> 16);
        in.shade.g = (uint8_t)clamp8(at[AG] >> 16);
        in.shade.b = (uint8_t)clamp8(at[AB] >> 16);
        in.shade.a = (uint8_t)clamp8(at[AA] >> 16);
    }
    if (pr->tex) {
        int32_t s, t;
        if (pr->texrect) { s = (int16_t)at[AS]; t = (int16_t)at[AT]; }   /* s10.5: wraps at 16 bits */
        else if (h & MRDP_H_PERSP) { s = mrdp_persp(at[AS], (uint32_t)at[AW]); t = mrdp_persp(at[AT], (uint32_t)at[AW]); }
        else { s = s105_noperp(at[AS]); t = s105_noperp(at[AT]); }
        in.tex = texture_sample(r, pr->tile, s, t);
    }

    /* combiner */
    mrdp_rgba_t comb;
    if (cyc == 2) {                                /* copy: the texel itself */
        comb = in.tex;
    } else if (cyc == 1) {
        in.combined = combine(r, &in, 0);
        comb = combine(r, &in, 1);
    } else {
        comb = combine(r, &in, 1);
    }

    /* alpha compare / coverage from alpha */
    if ((l & MRDP_L_ALPHA_CMP) && comb.a < r->blend.a) return;
    if ((l & MRDP_L_CVG_X_ALPHA) && comb.a < MRDP_CVG_ALPHA_MIN) return;

    /* blender */
    mrdp_rgba_t mem = { 0, 0, 0, 255 };
    int need_mem = cyc == 1 ? (bl_reads_memory(l, 0) || bl_reads_memory(l, 1)) : bl_reads_memory(l, 0);
    if (need_mem && cyc != 2) mem = unrgb565(r->rd16(r->ctx, caddr));
    mrdp_rgba_t out;
    if (cyc == 2) {
        out = comb;
    } else if (cyc == 1) {
        mrdp_rgba_t b0 = blend(r, 0, comb, mem, comb.a, in.shade.a);
        out = (l & MRDP_L_FORCE_BL) ? blend(r, 1, b0, mem, comb.a, in.shade.a)
                                    : bl_pm(r, (l >> 28) & 3, b0, mem);
    } else {
        out = (l & MRDP_L_FORCE_BL) ? blend(r, 0, comb, mem, comb.a, in.shade.a)
                                    : bl_pm(r, (l >> 30) & 3, comb, mem);
    }

#ifdef MRDP_DEBUG_PIXEL
    if (x == r->dbg_x && y == r->dbg_y) {
        fprintf(stderr, "MRDP pixel (%d,%d) <- %04x depth %04x op %02x words:", x, y, rgb565(out), depth, (r->cmd[0] >> 24) & 0x3F);
        for (int i = 0; i < r->ncmd; i++) fprintf(stderr, " %08x", r->cmd[i]);
        fprintf(stderr, "\n  at: r %08x g %08x b %08x a %08x s %08x t %08x w %08x z %08x\n",
                at[AR], at[AG], at[AB], at[AA], at[AS], at[AT], at[AW], at[AZ]);
    }
#endif
    r->wr16(r->ctx, caddr, rgb565(out));
    if (l & MRDP_L_Z_UPD) r->wr16(r->ctx, zaddr, depth);
    r->pixels_drawn++;
}

static inline int sc_x0(const mrdp_t *r) { return r->sc_xh >> 2; }
static inline int sc_x1(const mrdp_t *r) { return r->sc_xl >> 2; }
static inline int sc_y0(const mrdp_t *r) { return r->sc_yh >> 2; }
static inline int sc_y1(const mrdp_t *r) { return r->sc_yl >> 2; }

/* ---- triangle ------------------------------------------------------------ */

/* attribute: value, per-pixel and per-scanline (along the H edge) slopes */
typedef struct { int32_t v, dx, de; } attr_t;

static void tri_attrs(const uint32_t *w, attr_t *a, int base, int n)
{
    /* N64 layout: w[0] ints (A,B,C,D), w[2] ints of DxDx, w[4] fracs,
     * w[6] fracs of DxDx, w[8] ints of DxDe, w[10] ints of DxDy,
     * w[12] fracs of DxDe, w[14] fracs of DxDy -- 64-bit words as pairs */
    for (int k = 0; k < n; k++) {
        int wi = k >> 1, sh = (k & 1) ? 0 : 16;
        uint32_t vi = (w[0 + wi] >> sh) & 0xFFFF, vf = (w[4 + wi] >> sh) & 0xFFFF;
        uint32_t xi = (w[2 + wi] >> sh) & 0xFFFF, xf = (w[6 + wi] >> sh) & 0xFFFF;
        uint32_t ei = (w[8 + wi] >> sh) & 0xFFFF, ef = (w[12 + wi] >> sh) & 0xFFFF;
        a[base + k].v = (int32_t)(vi << 16 | vf);
        a[base + k].dx = (int32_t)(xi << 16 | xf);
        a[base + k].de = (int32_t)(ei << 16 | ef);
    }
}

/* AA_EN: the N64's coverage. Each row is four
 * subscanlines (y + j/4, j = 0..3; valid inside [YH, YL) in quarter pixels),
 * each with its own left / right edge -- here the row-centre edge plus
 * (j - 2) quarter slopes -- rounded to 1/8 px the N64's way (quarter-pixel
 * bits plus a sticky bit for anything finer). A pixel's samples: two per
 * subscanline in a checkerboard (0xA on even subscanlines, 0x5 on odd, bit 3
 * the leftmost of four quarter positions). A pixel is drawn when any sample
 * is covered, as the N64 does with AA on: two surfaces 0.22 px apart (WF's
 * ramp) no longer leave a column of sky. Attributes are the row's, at the
 * pixel centre; Z is moved to the first covered sample (the N64's offx/offy
 * correction) -- without that, a limb's edge pixels, their depth
 * extrapolated, drew over the body behind (Mario's arm on his overalls). */
static void draw_triangle_aa(mrdp_t *r, const prim_t *pr, int lft, int32_t yh, int32_t ym, int32_t yl,
                             int32_t xh, int32_t dxh, int32_t xm, int32_t dxm, int32_t xl, int32_t dxl,
                             const attr_t *at)
{
    int y0 = yh >> 2, ymr = ym >> 2;
    int ya = y0 > sc_y0(r) ? y0 : sc_y0(r);
    int yb = (yl + 3) >> 2;
    if (yb > sc_y1(r)) yb = sc_y1(r);
    const int32_t qh = dxh >> 2, qm = dxm >> 2, ql = dxl >> 2;
    /* dZ/dy at fixed x: DaDe steps along the H edge */
    const int32_t dzdy = (int32_t)((uint32_t)at[AZ].de - (uint32_t)(int32_t)(((int64_t)at[AZ].dx * dxh) >> 16));
    /* dz: how far one pixel moves the stored depth, |dZ/dx| + |dZ/dy| in its
     * units (Z >> 15) plus one -- the N64's per-primitive deltaZ */
    prim_t pz = *pr;
    {
        int32_t zx = at[AZ].dx >> 15, zy = dzdy >> 15;
        uint32_t dz = (uint32_t)(zx < 0 ? -zx : zx) + (uint32_t)(zy < 0 ? -zy : zy) + 1u;
        pz.dz = dz > 0xFFFFu ? 0xFFFFu : dz;
    }
    for (int y = ya; y < yb; y++) {
        uint32_t k = (uint32_t)(y - y0);
        int32_t exh = (int32_t)((uint32_t)xh + (uint32_t)(dxh >> 1) + k * (uint32_t)dxh);
        int32_t exm = (int32_t)((uint32_t)xm + (uint32_t)(dxm >> 1) + k * (uint32_t)dxm);
        int32_t exl = (int32_t)((uint32_t)xl + (uint32_t)(dxl >> 1) + (uint32_t)(y - ymr) * (uint32_t)dxl);
        int lp[4], rp[4], lf[4], rf[4], valid[4], any = 0;
        int xmin = 0, xmax = 0;
        for (int j = 0; j < 4; j++) {
            int yq = 4 * y + j;
            uint32_t m = (uint32_t)(j - 2);
            int32_t xa = (int32_t)((uint32_t)exh + m * (uint32_t)qh);
            int32_t xb = yq < ym ? (int32_t)((uint32_t)exm + m * (uint32_t)qm)
                                 : (int32_t)((uint32_t)exl + m * (uint32_t)ql);
            int32_t lx = lft ? xa : xb, rx = lft ? xb : xa;
            int32_t l8 = (int32_t)(((uint32_t)(lx >> 14) << 1) | ((lx & 0x3FFF) != 0));   /* 1/8 px, sticky */
            int32_t r8 = (int32_t)(((uint32_t)(rx >> 14) << 1) | ((rx & 0x3FFF) != 0));
            lp[j] = l8 >> 3; lf[j] = l8 & 7; rp[j] = r8 >> 3; rf[j] = r8 & 7;
            valid[j] = yq >= yh && yq < yl && r8 > l8;
            if (valid[j]) {
                if (!any || lp[j] < xmin) xmin = lp[j];
                if (!any || rp[j] > xmax) xmax = rp[j];
                any = 1;
            }
        }
#ifdef MRDP_DEBUG_AA
        if (any) {
            int v = 0; for (int j = 0; j < 4; j++) v |= valid[j] << j;
            fprintf(stderr, "AAROW y=%d v=%x", y, v);
            for (int j = 3; j >= 0; j--) fprintf(stderr, " %d:%d:%d:%d", lp[j] & 0xFFFF, lf[j], rp[j] & 0xFFFF, rf[j]);
            fprintf(stderr, " min=%d max=%d\n", xmin, xmax);
        }
#endif
        if (!any) continue;
        if (xmin < sc_x0(r)) xmin = sc_x0(r);
        if (xmax > sc_x1(r) - 1) xmax = sc_x1(r) - 1;
        /* the attributes at the span's first pixel, then a step per pixel --
         * exactly as the RTL (a fresh x - exh per pixel wraps differently
         * on a triangle whose H edge is 32 k px away) */
        int32_t dx0 = (int32_t)(((uint32_t)xmin << 16) + 0x8000) - exh;
        int32_t cur[NATTR];
        for (int i = 0; i < NATTR; i++) {
            uint32_t e = (uint32_t)at[i].v + (uint32_t)(at[i].de >> 1) + k * (uint32_t)at[i].de;
            cur[i] = (int32_t)(e + (uint32_t)(int32_t)(((int64_t)at[i].dx * dx0) >> 16));
        }
        for (int x = xmin; x <= xmax; x++) {
            if (x > xmin)
                for (int i = 0; i < NATTR; i++) cur[i] = (int32_t)((uint32_t)cur[i] + (uint32_t)at[i].dx);
            unsigned cj[4], cvg = 0;
            for (int j = 0; j < 4; j++) {
                unsigned fm = (j & 1) ? 0x5u : 0xAu;
                unsigned lm = (0xFu >> ((lf[j] + 1) >> 1)) & fm;
                unsigned rm = ((0xF0u >> ((rf[j] + 1) >> 1)) & 0xFu) & fm;
                cj[j] = 0;
                if (!valid[j]) continue;
                if (x > lp[j] && x < rp[j]) cj[j] = fm;
                else if (x == lp[j] && x == rp[j]) cj[j] = lm & rm;
                else if (x == lp[j]) cj[j] = lm;
                else if (x == rp[j]) cj[j] = rm;
                cvg |= cj[j];
            }
            if (!cvg) continue;
            int32_t pc[NATTR];
            for (int i = 0; i < NATTR; i++) pc[i] = cur[i];
            int oy = 0; while (!cj[oy]) oy++;
            int ox = 0; while (!((cj[oy] >> (3 - ox)) & 1u)) ox++;
            int32_t sx8 = 2 * ox - 4, sy8 = 2 * oy - 4;      /* the sample, from the centre, 1/8 px */
            uint32_t corr = ((uint32_t)((at[AZ].dx >> 5) * sx8) + (uint32_t)((dzdy >> 5) * sy8)) << 2;
            pc[AZ] = (int32_t)((uint32_t)pc[AZ] + corr);
#ifdef MRDP_DEBUG_AA
            fprintf(stderr, "AAPIX x=%d y=%d cvg=%x sx=%d sy=%d z=%08x\n", x, y, cvg, sx8, sy8, (uint32_t)pc[AZ]);
#endif
            unsigned cvgn = 0;
            for (int j = 0; j < 4; j++) cvgn += (unsigned)__builtin_popcount(cj[j]);
            draw_pixel(r, &pz, x, y, pc, (int)cvgn);
        }
    }
}

static void draw_triangle(mrdp_t *r, const uint32_t *w)
{
    unsigned op = (w[0] >> 24) & 0x3F;
    prim_t pr = { !!(op & 4), !!(op & 2), !!(op & 1), (int)((w[0] >> 16) & 7), 0 };
    int lft = (w[0] >> 23) & 1;
    int32_t yl = sext(w[0], 14), ym = sext(w[1] >> 16, 14), yh = sext(w[1], 14);
    int32_t xl = (int32_t)w[2], dxl = (int32_t)w[3];
    int32_t xh = (int32_t)w[4], dxh = (int32_t)w[5];
    int32_t xm = (int32_t)w[6], dxm = (int32_t)w[7];

    attr_t at[NATTR];
    memset(at, 0, sizeof at);
    const uint32_t *p = w + 8;
    if (pr.shade) { tri_attrs(p, at, AR, 4); p += 16; }
    if (pr.tex) { tri_attrs(p, at, AS, 3); p += 16; }   /* S, T, W (4th unused) */
    if (pr.zbuf) {
        at[AZ].v = (int32_t)p[0]; at[AZ].dx = (int32_t)p[1];
        at[AZ].de = (int32_t)p[2];
    }

    int y0 = yh >> 2;                 /* the integer scanline of XH / XM / A */
    int ymr = ym >> 2;                /* the integer scanline of XL */
    int ya = y0 > sc_y0(r) ? y0 : sc_y0(r);
    int yb = ((yl - 2 + 3) >> 2);     /* rows with 4y+2 < yl: y <= (yl-3)/4 */
    if (yb > sc_y1(r)) yb = sc_y1(r);

#ifdef MRDP_COVERAGE
    if (r->other_l & MRDP_L_AA_EN) { draw_triangle_aa(r, &pr, lft, yh, ym, yl, xh, dxh, xm, dxm, xl, dxl, at); return; }
#endif
    /* AA_EN without MRDP_COVERAGE: the crack grow (mrdp_grow_mask, TRI_R only) */
    int32_t gh = 0, gm = 0, gl = 0;
    if (r->other_l & MRDP_L_AA_EN) {
        if (r->grow & 1u) gh = mrdp_grow_amount(dxh);
        if (r->grow & 2u) gm = mrdp_grow_amount(dxm);
        if (r->grow & 4u) gl = mrdp_grow_amount(dxl);
    }
    for (int y = ya; y < yb; y++) {
        if (4 * y + 2 < yh) continue;
        /* all edge and attribute arithmetic wraps modulo 2^32, as the RTL's
         * incremental adders do */
        uint32_t k = (uint32_t)(y - y0);
        int32_t exh = (int32_t)((uint32_t)xh + (uint32_t)(dxh >> 1) + k * (uint32_t)dxh);
        int32_t exm = (4 * y + 2 < ym)
            ? (int32_t)((uint32_t)xm + (uint32_t)(dxm >> 1) + k * (uint32_t)dxm)
            : (int32_t)((uint32_t)xl + (uint32_t)(dxl >> 1) + (uint32_t)(y - ymr) * (uint32_t)dxl);
        int32_t xleft = lft ? exh : exm, xright = lft ? exm : exh;
        int32_t gmin = (4 * y + 2 < ym) ? gm : gl;
        int32_t gleft = lft ? gh : gmin, gright = lft ? gmin : gh;
        /* 33 bits, as the RTL (xs_c / xe_c): in 32 an edge wrapped near
         * +-32768 px overflowed and the model and the RTL disagreed */
        int64_t xs64 = ((int64_t)xleft + (0x7FFF - gleft)) >> 16, xe64 = ((int64_t)xright + (0x7FFF + gright)) >> 16;
        if (xs64 < sc_x0(r)) xs64 = sc_x0(r);
        if (xe64 > sc_x1(r)) xe64 = sc_x1(r);
        int xs = (int)xs64, xe = (int)xe64;
        if (xs >= xe) continue;

        /* span start: A + DaDe/2 + k*DaDe on the H edge, then DaDx * (xs + 0.5 - xh) */
        int32_t dx = (int32_t)(((uint32_t)xs << 16) + 0x8000) - exh;
        int32_t cur[NATTR];
        for (int i = 0; i < NATTR; i++) {
            uint32_t e = (uint32_t)at[i].v + (uint32_t)(at[i].de >> 1) + k * (uint32_t)at[i].de;
            cur[i] = (int32_t)(e + (uint32_t)(int32_t)(((int64_t)at[i].dx * dx) >> 16));
        }
        for (int x = xs; x < xe; x++) {
            draw_pixel(r, &pr, x, y, cur, 8);
            for (int i = 0; i < NATTR; i++) cur[i] = (int32_t)((uint32_t)cur[i] + (uint32_t)at[i].dx);
        }
    }
}

/* ---- rectangles ----------------------------------------------------------- */

static void draw_texrect(mrdp_t *r, const uint32_t *w, int flip)
{
    unsigned cyc = (r->other_h >> MRDP_H_CYCLE_SHIFT) & 3;
    int xl = (w[0] >> 12) & 0xFFF, yl = w[0] & 0xFFF;
    int xh = (w[1] >> 12) & 0xFFF, yh = w[1] & 0xFFF;
    prim_t pr = { 0, 1, 0, (int)((w[1] >> 24) & 7), 1 };
    int32_t s = sext(w[2] >> 16, 16), t = sext(w[2], 16);
    int32_t dsdx = sext(w[3] >> 16, 16), dtdy = sext(w[3], 16);
    int x0 = xh >> 2, x1 = (cyc == 2) ? (xl >> 2) + 1 : (xl >> 2);
    int y0 = yh >> 2, y1 = (cyc == 2) ? (yl >> 2) + 1 : (yl >> 2);
    int dshift = (cyc == 2) ? 7 : 5;              /* copy mode: DsDx is 4x */
    int xa = x0 > sc_x0(r) ? x0 : sc_x0(r), xb = x1 < sc_x1(r) ? x1 : sc_x1(r);
    int ya = y0 > sc_y0(r) ? y0 : sc_y0(r), yb = y1 < sc_y1(r) ? y1 : sc_y1(r);
    for (int y = ya; y < yb; y++)
        for (int x = xa; x < xb; x++) {
            int32_t at[NATTR] = { 0 };
            int32_t ds = (dsdx * (x - x0)) >> dshift, dt = (dtdy * (y - y0)) >> 5;
            at[AS] = flip ? s + ((dsdx * (y - y0)) >> dshift) : s + ds;
            at[AT] = flip ? t + ((dtdy * (x - x0)) >> 5) : t + dt;
            draw_pixel(r, &pr, x, y, at, 8);
        }
}

static void fill_rect(mrdp_t *r, const uint32_t *w)
{
    unsigned cyc = (r->other_h >> MRDP_H_CYCLE_SHIFT) & 3;
    int xl = (w[0] >> 12) & 0xFFF, yl = w[0] & 0xFFF;
    int xh = (w[1] >> 12) & 0xFFF, yh = w[1] & 0xFFF;
    int x0 = xh >> 2, y0 = yh >> 2;
    int x1 = (cyc == 3) ? (xl >> 2) + 1 : (xl >> 2);
    int y1 = (cyc == 3) ? (yl >> 2) + 1 : (yl >> 2);
    prim_t pr = { 0, 0, 0, 0, 0 };
    int xa = x0 > sc_x0(r) ? x0 : sc_x0(r), xb = x1 < sc_x1(r) ? x1 : sc_x1(r);
    int ya = y0 > sc_y0(r) ? y0 : sc_y0(r), yb = y1 < sc_y1(r) ? y1 : sc_y1(r);
    int32_t at[NATTR] = { 0 };
    for (int y = ya; y < yb; y++)
        for (int x = xa; x < xb; x++)
            draw_pixel(r, &pr, x, y, at, 8);
}

/* ---- texture loads ---------------------------------------------------------- */

static void set_tile_size(mrdp_t *r, const uint32_t *w)
{
    mrdp_tile_t *t = &r->tile[(w[1] >> 24) & 7];
    t->uls = (w[0] >> 12) & 0xFFF;
    t->ult = w[0] & 0xFFF;
    t->lrs = (w[1] >> 12) & 0xFFF;
    t->lrt = w[1] & 0xFFF;
}

static void load_tile(mrdp_t *r, const uint32_t *w)
{
    set_tile_size(r, w);
    const mrdp_tile_t *t = &r->tile[(w[1] >> 24) & 7];
    int s0 = t->uls >> 2, t0 = t->ult >> 2, s1 = t->lrs >> 2, t1 = t->lrt >> 2;
    for (int tt = t0; tt <= t1; tt++)
        for (int s = s0; s <= s1; s++) {
            uint16_t v = r->rd16(r->ctx, r->timg + ((uint32_t)tt * r->timg_width + (uint32_t)s) * 2);
            int bank, addr;
            tmem_locate(t, s - s0, tt - t0, &bank, &addr);
            r->tmem[bank][addr] = v;
        }
}

static void set_tile(mrdp_t *r, const uint32_t *w)
{
    mrdp_tile_t *t = &r->tile[(w[1] >> 24) & 7];
    t->fmt = (w[0] >> 21) & 7;
    t->line = (w[0] >> 9) & 0x1FF;
    t->tmem = (uint16_t)((w[0] & 0x1FF) | (((w[0] >> 18) & 1) << 9));
    t->ct = (w[1] >> 19) & 1; t->mt = (w[1] >> 18) & 1;
    t->maskt = (w[1] >> 14) & 15; t->shiftt = (w[1] >> 10) & 15;
    t->cs = (w[1] >> 9) & 1; t->ms = (w[1] >> 8) & 1;
    t->masks = (w[1] >> 4) & 15; t->shifts = w[1] & 15;
}

/* ---- command dispatch --------------------------------------------------------- */

/* TRI_V / TRI_G (mrdp_setup.h): the attributes' value, DaDx and DaDe, from
 * the vertex values as the RTL computes them (mrdp_vattr) or as sent, then
 * the N64 triangle they make. (DaDy is not used by the walker.) */
static void draw_triangle_vg(mrdp_t *r, const uint32_t *w, int g)
{
    unsigned flags = (w[0] >> 24) & 7u;
    int32_t F[4] = { (int32_t)w[8], (int32_t)w[9], (int32_t)w[10], (int32_t)w[11] };
    int32_t kx = (int32_t)w[12], ky = (int32_t)w[13], dxh = (int32_t)w[5];
    int32_t val[8], ddx[8], dde[8], ddy[8];
    const uint32_t *a = w + (g ? 8 : 14);
    for (int i = 0; i < 8; i++) {
        val[i] = ddx[i] = dde[i] = ddy[i] = 0;
        if (!mrdp_attr_on(flags, i)) continue;
        if (g) { val[i] = (int32_t)a[0]; ddx[i] = (int32_t)a[1]; dde[i] = (int32_t)a[2]; }
        else mrdp_vattr(F, kx, ky, dxh, (int32_t)a[0], (int32_t)a[1], (int32_t)a[2], &val[i], &ddx[i], &dde[i], &ddy[i]);
        a += 3;
    }
    uint32_t t[44];
    t[0] = ((0x08u | flags) << 24) | (w[0] & 0x00FFFFFFu);
    for (int k = 1; k < 8; k++) t[k] = w[k];
    mrdp_put_attrs(t, 8, flags, val, ddx, dde, ddy);
    draw_triangle(r, t);
}

/* TRI_R (mrdp_setup.h): MRDP's whole setup, mrdp_tri_r() + mrdp_tri_r_attr() */
static void draw_triangle_r(mrdp_t *r, const uint32_t *w)
{
    unsigned flags = (w[0] >> 19) & 7u, tile = (w[0] >> 16) & 7u;
    int32_t xy[6], kx, ky;
    for (int k = 0; k < 6; k++) xy[k] = (int32_t)w[1 + k];
    mrdp_edges_t E;
    mrdp_fac_t fc;
    if (!mrdp_tri_r(xy, &E, &fc, &kx, &ky)) return;
    r->grow = mrdp_grow_mask(&E);
    int32_t av[8][3] = { { 0 } };
    const uint32_t *a = w + 7;
    for (int i = 0; i < 8; i++) {
        if (!mrdp_attr_on(flags, i)) continue;
        av[i][0] = (int32_t)a[0]; av[i][1] = (int32_t)a[1]; av[i][2] = (int32_t)a[2];
        a += 3;
    }
    mrdp_tri_r_fields(av, flags);
    int32_t val[8], ddx[8], dde[8], ddy[8];
    for (int i = 0; i < 8; i++) {
        val[i] = ddx[i] = dde[i] = ddy[i] = 0;
        if (!mrdp_attr_on(flags, i)) continue;
        mrdp_tri_r_attr(&fc, kx, ky, E.dxh, av[i][0], av[i][1], av[i][2], &val[i], &ddx[i], &dde[i], &ddy[i]);
    }
    uint32_t t[44];
    mrdp_put_edges(t, &E, (0x08u | flags) << 24, tile);
    mrdp_put_attrs(t, 8, flags, val, ddx, dde, ddy);
    draw_triangle(r, t);
}

static void execute(mrdp_t *r, const uint32_t *w)
{
    unsigned op = (w[0] >> 24) & 0x3F;
    r->grow = 0;                                  /* TRI_R sets it (draw_triangle_r) */
    if (op >= 0x08 && op <= 0x0F) { draw_triangle(r, w); return; }
    if (op >= MRDP_OP_TRI_V && op <= (MRDP_OP_TRI_V | 7u)) { draw_triangle_vg(r, w, 0); return; }
    if (op >= MRDP_OP_TRI_G && op <= (MRDP_OP_TRI_G | 7u)) { draw_triangle_vg(r, w, 1); return; }
    if (op == MRDP_OP_TRI_R) { draw_triangle_r(r, w); return; }
    switch (op) {
    case MRDP_OP_NOP: case MRDP_OP_SYNC_LOAD: case MRDP_OP_SYNC_PIPE: case MRDP_OP_SYNC_TILE:
        break;
    case MRDP_OP_SYNC_FULL: r->sync_count++; break;
    case MRDP_OP_TEXRECT: draw_texrect(r, w, 0); break;
    case MRDP_OP_TEXRECT_FLIP: draw_texrect(r, w, 1); break;
    case MRDP_OP_SET_SCISSOR:
        r->sc_xh = (w[0] >> 12) & 0xFFF; r->sc_yh = w[0] & 0xFFF;
        r->sc_xl = (w[1] >> 12) & 0xFFF; r->sc_yl = w[1] & 0xFFF;
        break;
    case MRDP_OP_SET_PRIM_Z: r->prim_z = (uint16_t)(w[1] >> 16); break;
    case MRDP_OP_SET_OTHER: r->other_h = w[0] & 0xFFFFFF; r->other_l = w[1]; break;
    case MRDP_OP_SET_TILESIZE: set_tile_size(r, w); break;
    case MRDP_OP_LOAD_TILE: load_tile(r, w); r->load_count++; break;
    case MRDP_OP_SET_TILE: set_tile(r, w); break;
    case MRDP_OP_FILL_RECT: fill_rect(r, w); break;
    case MRDP_OP_SET_FILL: r->fill = w[1]; break;
    case MRDP_OP_SET_FOG: r->fog = rgba_of(w[1]); break;
    case MRDP_OP_SET_BLEND: r->blend = rgba_of(w[1]); break;
    case MRDP_OP_SET_PRIM: r->prim = rgba_of(w[1]); r->prim_lod_frac = (uint8_t)w[0]; break;
    case MRDP_OP_SET_ENV: r->env = rgba_of(w[1]); break;
    case MRDP_OP_SET_COMBINE: r->combine_hi = w[0] & 0xFFFFFF; r->combine_lo = w[1]; break;
    case MRDP_OP_SET_TIMG: r->timg = w[1]; r->timg_width = (w[0] & 0x3FF) + 1; break;
    case MRDP_OP_SET_ZIMG: r->zimg = w[1]; break;
    case MRDP_OP_SET_CIMG: r->cimg = w[1]; r->cimg_width = (w[0] & 0x3FF) + 1; break;
    default: r->unknown_ops++; break;
    }
}

void mrdp_push(mrdp_t *r, uint32_t word)
{
    if (r->ncmd == 0)
        r->need = 2 * mrdp_cmd_len(word);
    r->cmd[r->ncmd++] = word;
    if (r->ncmd == r->need) {
        execute(r, r->cmd);
        r->ncmd = 0;
    }
}
