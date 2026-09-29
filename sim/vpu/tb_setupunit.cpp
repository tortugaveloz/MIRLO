// Verilator testbench for rtl/vpu/GeomSetupUnit.v, driven through the real
// CFU wrapper (Vpu4DFixed), against the C it replaces, copied verbatim:
// lang/c/geom/geom_triangle.c round_to_float()/blend_attr() and
// lang/c/geom/geom_fixed.c fx_recip_norm(). Bit-exact or it fails.
//
// Build/run: make -f Makefile.setup

#include <verilated.h>
#include "VVpu4DFixed.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

static VVpu4DFixed *dut;
/* PUSH (0x61) port: words taken on each rising edge with out_valid & out_ready */
static int g_rand_ready;                 /* out_ready toggles pseudo-randomly */
static uint32_t g_pushed[128]; static int g_npushed;
static uint32_t g_lfsr = 0xACE1u;
static void tick() {
    dut->clk = 0;
    if (g_rand_ready) { g_lfsr = g_lfsr * 1103515245u + 12345u; dut->out_ready = (g_lfsr >> 16) & 1; }
    else dut->out_ready = 1;
    dut->eval();
    if (dut->out_valid && dut->out_ready && g_npushed < 128) g_pushed[g_npushed++] = dut->out_data;
    dut->clk = 1; dut->eval();
}

static uint32_t cfu(uint32_t fid, uint32_t in0, uint32_t in1, int *lat = nullptr) {
    dut->cmd_function_id = fid; dut->cmd_inputs_0 = in0; dut->cmd_inputs_1 = in1;
    dut->cmd_valid = 1; dut->rsp_ready = 1;
    int g = 0, n = 0;
    while (!dut->cmd_ready) { tick(); n++; if (++g > 100) { printf("TIMEOUT accept %x\n", fid); exit(2); } }
    tick(); n++;
    dut->cmd_valid = 0;
    g = 0;
    while (!dut->rsp_valid) { tick(); n++; if (++g > 1000) { printf("TIMEOUT rsp %x\n", fid); exit(2); } }
    uint32_t r = dut->rsp_outputs_0;
    tick(); n++;
    if (lat) *lat = n;
    return r;
}

// ---- reference: verbatim from geom_triangle.c ------------------------------
static uint32_t f2u_(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static float round_to_float(int64_t p)
{
    if (p == 0) return 0.0f;
    uint32_t sign = (p < 0) ? 0x80000000u : 0u;
    uint64_t u = (p < 0) ? 0u - (uint64_t)p : (uint64_t)p;
    if (u >= ((uint64_t)1 << 47)) return u2f(sign | 0x47000000u);   /* +-32768 */
    uint32_t hi = (uint32_t)(u >> 32), lo = (uint32_t)u;            /* hi < 2^15 */
    int msb = 0; uint32_t t = hi ? hi : lo;
    if (t >> 16) { t >>= 16; msb += 16; }
    if (t >>  8) { t >>=  8; msb +=  8; }
    if (t >>  4) { t >>=  4; msb +=  4; }
    if (t >>  2) { t >>=  2; msb +=  2; }
    if (t >>  1) {           msb +=  1; }
    if (hi) msb += 32;
    int shift = msb - 23;
    uint32_t mant;
    if (shift <= 0) {
        mant = lo << (-shift);
    } else {
        uint32_t half = 1u << (shift - 1);
        uint32_t lo2 = lo + half;
        uint32_t hi2 = hi + (lo2 < lo);
        mant = (hi2 << (32 - shift)) | (lo2 >> shift);
    }
    if (mant & 0x1000000u) { mant >>= 1; msb++; }
    return u2f(sign | ((uint32_t)(msb + 127 - 32) << 23) | (mant & 0x7FFFFFu));
}
// verbatim from geom_triangle.c: p / 2^s, halves away from zero, saturated
static int32_t round_to_fixed(int64_t p, int s)
{
    uint64_t h = (uint64_t)1 << (s - 1);
    int64_t q = (int64_t)((uint64_t)p + (p < 0 ? h - 1 : h)) >> s;
    if (q > 0x7FFFFFFF) return 0x7FFFFFFF;
    if (q < -(int64_t)0x80000000) return (int32_t)0x80000000;
    return (int32_t)q;
}
static int32_t sat_bits(int64_t v, int bits) {
    int64_t hi = ((int64_t)1 << (bits - 1)) - 1;
    return (int32_t)(v > hi ? hi : v < -hi - 1 ? -hi - 1 : v);
}
static void blend_attr(int32_t *v, int32_t *vx, int32_t *vy, int32_t a0, int32_t a1, int32_t a2,
                       const int32_t wN[3], const int32_t wXN[3], const int32_t wYN[3], int s)
{
    int64_t d1 = sat_bits((int64_t)a1 - a0, 20), d2 = sat_bits((int64_t)a2 - a0, 20);
    *v  = round_to_fixed(((int64_t)a0 << 16) + d1 * sat_bits(wN[1], 27)  + d2 * sat_bits(wN[2], 27),  s);
    *vx = round_to_fixed(d1 * sat_bits(wXN[1], 27) + d2 * sat_bits(wXN[2], 27), s);
    *vy = round_to_fixed(d1 * sat_bits(wYN[1], 27) + d2 * sat_bits(wYN[2], 27), s);
}

// ---- reference: verbatim from geom_fixed.c ---------------------------------
typedef struct { uint32_t r; int e; } fx_recip_norm_t;
#include "../../lang/c/geom/geom_rcp_tab.h"
static fx_recip_norm_t fx_recip_norm(uint32_t a)
{
    int msb = 0; uint32_t t = a;
    if (t >> 16) { t >>= 16; msb += 16; }
    if (t >>  8) { t >>=  8; msb +=  8; }
    if (t >>  4) { t >>=  4; msb +=  4; }
    if (t >>  2) { t >>=  2; msb +=  2; }
    if (t >>  1) {           msb +=  1; }
    int s = msb - 9;
    if (s < 16) s = 16;
    uint32_t m   = a << (31 - msb);
    int32_t  r0  = (int32_t)((0x10000u | geom_rcp_tab[(m >> 22) & 511u]) << 6);
    uint64_t e46 = (uint64_t)(m >> 8) * (uint32_t)r0;
    int64_t  eps = (int64_t)(1ull << 46) - (int64_t)e46;
    int32_t  r1  = r0 + (int32_t)(((int64_t)r0 * (eps >> 10)) >> 36);
    fx_recip_norm_t k = { (uint32_t)r1 << (9 + s - msb), s - 16 };
    return k;
}

static int32_t fx_clamp(int64_t v) {
    if (v > 0x7FFFFFFF) return 0x7FFFFFFF;
    if (v < (int64_t)(int32_t)0x80000000) return (int32_t)0x80000000;
    return (int32_t)v;
}
static int32_t fx_mul(int32_t a, int32_t b) {
    int64_t p = (int64_t)a * (int64_t)b;
    int64_t half = 1LL << 15;
    p = (p >= 0) ? (p + half) >> 16 : -(((-p) + half) >> 16);
    return fx_clamp(p);
}
static int32_t fx_div_norm(int32_t x, fx_recip_norm_t k) {
    int64_t p = (int64_t)x * (int64_t)k.r;
    int32_t hi = (int32_t)(p >> 32);
    if (k.e == 0) return hi + (int32_t)((uint32_t)p >> 31);
    return (hi >> k.e) + ((hi >> (k.e - 1)) & 1);
}

// verbatim from geom_pipeline.c
static uint32_t isqrt_u32(uint32_t x)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > x) b >>= 2;
    while (b) {
        if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
        else            r >>= 1;
        b >>= 2;
    }
    return r;
}

// verbatim from geom_fixed.c
static int32_t fx_from_bits_n(uint32_t bits, int frac_bits, int32_t max_raw, int32_t min_raw) {
    uint32_t sign = bits >> 31;
    int32_t  exp  = (int32_t)((bits >> 23) & 0xFFu);
    int32_t  v;
    if (exp == 0) return 0;
    if (exp == 0xFF) return sign ? -max_raw : max_raw;
    uint32_t m24 = 0x800000u | (bits & 0x7FFFFFu);
    int shift = exp - 127 - 23 + frac_bits;
    if (shift >= 0) {
        if (shift >= 8) return sign ? min_raw : max_raw;
        uint32_t u = m24 << shift;
        if (u > (uint32_t)max_raw) return sign ? min_raw : max_raw;
        v = (int32_t)u;
    } else {
        int s = -shift;
        v = (s >= 32) ? 0 : (int32_t)((m24 + (1u << (s - 1))) >> s);
    }
    if (sign) v = -v;
    if (v > max_raw) v = max_raw;
    if (v < min_raw) v = min_raw;
    return v;
}
static uint32_t fx_to_bits_n(int32_t fx, int frac_bits) {
    if (fx == 0) return 0;
    uint32_t sign = (fx < 0) ? (1u << 31) : 0;
    uint32_t m = (fx < 0) ? 0u - (uint32_t)fx : (uint32_t)fx;
    int msb = 0; uint32_t t = m;
    if (t >> 16) { t >>= 16; msb += 16; }
    if (t >>  8) { t >>=  8; msb +=  8; }
    if (t >>  4) { t >>=  4; msb +=  4; }
    if (t >>  2) { t >>=  2; msb +=  2; }
    if (t >>  1) {           msb +=  1; }
    int shift = msb - 23;
    int exp = msb - frac_bits + 127;
    uint32_t mant24;
    if (shift >= 0) {
        uint32_t half = (shift > 0) ? (1u << (shift - 1)) : 0;
        mant24 = (m + half) >> shift;
        if (mant24 & 0x1000000u) { mant24 >>= 1; exp++; }
    } else {
        mant24 = m << (-shift);
    }
    if (exp <= 0) return sign;
    if (exp >= 255) return sign | (0xFFu << 23);
    return sign | ((uint32_t)exp << 23) | (mant24 & 0x7FFFFFu);
}

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd32() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)g_rng; }
static int32_t rnd_range(int bits) {            // signed value with up to `bits` magnitude bits
    int b = (int)(rnd32() % (unsigned)(bits + 1));
    uint32_t v = b ? (rnd32() & ((b >= 32) ? 0xFFFFFFFFu : ((1u << b) - 1u))) : 0u;
    return (rnd32() & 1) ? -(int32_t)v : (int32_t)v;
}

static int g_fail = 0;
static long g_n = 0;

static const int SHIFTS[3] = {8, 2, 4};   /* colour, depth/w, texture */
static void test_blend(const int32_t W[6], int32_t a0, int32_t a1, int32_t a2, int *lat) {
    int s = SHIFTS[(g_n) % 3];
    for (int i = 0; i < 6; i++) cfu(0x04, (uint32_t)W[i], (uint32_t)i);
    cfu(0x40, (uint32_t)a1, (uint32_t)a2);
    // X and Y only come back through the output buffer (deposit + 0x5A)
    uint32_t hv = cfu(0x46, (uint32_t)a0, (uint32_t)s | 40u << 8 | 41u << 16 | 42u << 24, lat);
    uint32_t hx = cfu(0x5A, 0, 41), hy = cfu(0x5A, 0, 42);
    int32_t wN[3] = {0, W[0], W[1]}, wXN[3] = {0, W[2], W[3]}, wYN[3] = {0, W[4], W[5]};
    int32_t v, vx, vy;
    blend_attr(&v, &vx, &vy, a0, a1, a2, wN, wXN, wYN, s);
    g_n++;
    if (hv != (uint32_t)v || hx != (uint32_t)vx || hy != (uint32_t)vy) {
        if (g_fail < 10)
            printf("BLEND MISMATCH a=(%d,%d,%d) W=(%d,%d,%d,%d,%d,%d): hw %08x %08x %08x  ref %08x %08x %08x\n",
                   a0, a1, a2, W[0], W[1], W[2], W[3], W[4], W[5], hv, hx, hy, (uint32_t)v, (uint32_t)vx, (uint32_t)vy);
        g_fail++;
    }
}

static int g_lat_div = 0;
static void test_recip(uint32_t a, int *lat) {
    uint32_t r = cfu(0x50, a, 0, lat);
    uint32_t e = cfu(0x51, 0, 0);
    fx_recip_norm_t k = fx_recip_norm(a);
    g_n++;
    if (r != k.r || (int)e != k.e) {
        if (g_fail < 10) printf("RECIP MISMATCH a=%u: hw r=%u e=%u  ref r=%u e=%d\n", a, r, e, k.r, k.e);
        g_fail++;
    }
    // then divisions by that divisor, as divw()/the barycentric weights do
    for (int i = 0; i < 4; i++) {
        int32_t x = (i == 0) ? (int32_t)0x80000000 : (i == 1) ? 0x7FFFFFFF : (int32_t)rnd32() >> (rnd32() % 24);
        int32_t hw = (int32_t)cfu(0x53, (uint32_t)x, k.r, &g_lat_div);
        int32_t want = fx_div_norm(x, k);
        g_n++;
        if (hw != want) {
            if (g_fail < 10) printf("DIVN MISMATCH x=%d a=%u: hw %d ref %d\n", x, a, hw, want);
            g_fail++;
        }
    }
}
static int g_lat_b3 = 0, g_lat_nf = 0;
// verbatim from geom_pipeline.c normal_f()'s C
static int32_t normal_f_ref(uint32_t len2) {
    if (!len2) return 0;
    int msb = 0; uint32_t t = len2;
    if (t >> 8) { t >>= 8; msb += 8; }
    if (t >> 4) { t >>= 4; msb += 4; }
    if (t >> 2) { t >>= 2; msb += 2; }
    if (t >> 1) {           msb += 1; }
    int32_t r = geom_rsq_tab[((uint32_t)(msb & 1) << 8) | (((len2 << (15 - msb)) >> 7) & 255u)];
    int sh = 4 - ((msb & ~1) >> 1);
    return sh >= 0 ? r << sh : r >> -sh;
}
static void test_normf(uint32_t len2) {
    uint32_t hw = cfu(0x54, len2, 0, &g_lat_nf);
    uint32_t want = (uint32_t)normal_f_ref(len2);
    g_n++;
    if (hw != want) {
        if (g_fail < 10) printf("NORMF MISMATCH len2=%u: hw %u ref %u\n", len2, hw, want);
        g_fail++;
    }
}
static int g_lat_cv = 0;
static void test_f2fx(uint32_t bits) {
    for (int mode = 0; mode < 2; mode++) {
        int32_t hw = (int32_t)cfu(0x56, bits, (uint32_t)mode, &g_lat_cv);
        int32_t want = mode ? fx_from_bits_n(bits, 10, (1 << 26) - 1, -(1 << 26))
                            : fx_from_bits_n(bits, 16, 0x7FFFFFFF, (int32_t)0x80000000);
        g_n++;
        if (hw != want) {
            if (g_fail < 10) printf("F2FX MISMATCH bits=%08x mode=%d: hw %d ref %d\n", bits, mode, hw, want);
            g_fail++;
        }
    }
}
static void test_fx2f(int32_t v) {
    uint32_t hw = cfu(0x55, (uint32_t)v, 0, &g_lat_cv);
    uint32_t want = fx_to_bits_n(v, 16);
    g_n++;
    if (hw != want) {
        if (g_fail < 10) printf("FX2F MISMATCH v=%d: hw %08x ref %08x\n", v, hw, want);
        g_fail++;
    }
}
// DIVN with a software reciprocal: e set through 0x57, r passed explicitly
static void test_divn_sw(uint32_t a) {
    fx_recip_norm_t k = fx_recip_norm(a);
    cfu(0x57, (uint32_t)k.e, 0);
    for (int i = 0; i < 4; i++) {
        int32_t x = (int32_t)rnd32() >> (rnd32() % 24);
        int32_t hw = (int32_t)cfu(0x53, (uint32_t)x, k.r);
        int32_t want = fx_div_norm(x, k);
        g_n++;
        if (hw != want) {
            if (g_fail < 10) printf("DIVN(sw k) MISMATCH x=%d a=%u: hw %d ref %d\n", x, a, hw, want);
            g_fail++;
        }
    }
}
static int g_lat_mul = 0;
static void test_mul(int32_t a, int32_t b) {
    int32_t hw = (int32_t)cfu(0x52, (uint32_t)a, (uint32_t)b, &g_lat_mul);
    int32_t want = fx_mul(a, b);
    g_n++;
    if (hw != want) {
        if (g_fail < 10) printf("FXMUL MISMATCH %d*%d: hw %d ref %d\n", a, b, hw, want);
        g_fail++;
    }
}

// ---- PROJECT (0x58..0x5A): reference = load_vertices()'s steps ----------
static int32_t colfx_ref(int32_t raw, int sh) {
    if (raw > (0x7FFFFFFF >> sh)) return 0x7FFFFFFF;
    if (raw < ((int32_t)0x80000000 >> sh)) return (int32_t)0x80000000;
    return (int32_t)((uint32_t)raw << sh);
}
static int32_t fx_add_ref(int32_t a, int32_t b) { return fx_clamp((int64_t)a + b); }
static int32_t fx_sub_ref(int32_t a, int32_t b) { return fx_clamp((int64_t)a - b); }
static int g_lat_proj;
static void test_project(const int32_t M[16], const int32_t A[4], const int sh[4],
                         int32_t sx, int32_t tx, int32_t sy, int32_t ty) {
    for (int i = 0; i < 16; i++) cfu(0x00, (uint32_t)M[i], i);
    for (int i = 0; i < 4; i++) cfu(0x01, (uint32_t)A[i], i);
    cfu(0x30, 0, 0);
    int32_t raw[4];
    for (int j = 0; j < 4; j++) raw[j] = (int32_t)cfu(0x08, 0, j);
    cfu(0x58, (uint32_t)sx, 0); cfu(0x58, (uint32_t)tx, 1);
    cfu(0x58, (uint32_t)sy, 2); cfu(0x58, (uint32_t)ty, 3);
    cfu(0x58, (uint32_t)(sh[0] | sh[1] << 4 | sh[2] << 8 | sh[3] << 12), 4);
    int lat;
    int32_t w = (int32_t)cfu(0x59, 0, 0, &lat);
    int32_t c[4];
    for (int j = 0; j < 4; j++) c[j] = colfx_ref(raw[j], sh[j]);
    uint32_t aw = c[3] < 0 ? 0u - (uint32_t)c[3] : (uint32_t)c[3];
    int32_t want[6] = {0, 0, 0, c[0], c[1], c[2]};
    int norm = aw >= (1u << 17);
    if (norm) {
        if (lat > g_lat_proj) g_lat_proj = lat;
        fx_recip_norm_t k = fx_recip_norm(aw);
        int32_t q[3];
        for (int j = 0; j < 3; j++) {
            int32_t d = fx_div_norm(c[j], k);
            q[j] = c[3] < 0 ? (int32_t)(0u - (uint32_t)d) : d;
        }
        want[0] = fx_add_ref(fx_mul(q[0], sx), tx);
        want[1] = fx_sub_ref(ty, fx_mul(q[1], sy));
        want[2] = fx_mul(fx_add_ref(fx_mul(q[2], 32768), 32768), 65534);
    }
    g_n++;
    int bad = (w != c[3]);
    for (int i = norm ? 0 : 3; i < 6; i++) {
        int32_t got = (int32_t)cfu(0x5A, 0, 56 + i);   // out[56..61]
        if (got != want[i]) { bad = 1;
            if (g_fail < 10) printf("PROJECT mismatch item %d: got %08x want %08x (w %08x sh %d%d%d%d)\n",
                                    i, (unsigned)got, (unsigned)want[i], (unsigned)c[3], sh[0], sh[1], sh[2], sh[3]); }
    }
    if (bad) g_fail++;
}

/* 0x60 DWR, 0x46/0x47 blends that deposit V/X/Y, 0x61 PUSH: what comes out
 * of the port is exactly the buffer the ops built, in order, whatever
 * out_ready does. */
static int g_ob_fail, g_ob_runs;
static void test_output_buffer(int rand_ready)
{
    uint32_t ref[64];
    for (int i = 0; i < 64; i++) { ref[i] = rnd32(); cfu(0x60, ref[i], (uint32_t)i); }
    for (int k = 0; k < 12; k++) {
        int32_t W[9];
        for (int i = 0; i < 6; i++) { W[i] = rnd_range(20); cfu(0x04, (uint32_t)W[i], (uint32_t)i); }
        int32_t a0 = rnd_range(24), a1 = rnd_range(24), a2 = rnd_range(24);
        uint32_t iv = rnd32() & 63, ix = rnd32() & 63, iy = rnd32() & 63;
        if (ix == iv) ix = (iv + 1) & 63;
        if (iy == iv || iy == ix) iy = (ix + 1) & 63;
        if (iy == iv) iy = (iv + 2) & 63;
        uint32_t s = (k & 1) ? 8 : ((k & 2) ? 4 : 2);
        cfu(0x40, (uint32_t)a1, (uint32_t)a2);
        uint32_t fid = 0x46;
        uint32_t v = cfu(fid, (uint32_t)a0, s | iv << 8 | ix << 16 | iy << 24);
        ref[iv] = v; ref[ix] = cfu(0x5A, 0, ix); ref[iy] = cfu(0x5A, 0, iy);   // (values checked by test_blend)
    }
    uint32_t n = 1 + (rnd32() % 64);
    g_rand_ready = rand_ready; g_npushed = 0;
    cfu(0x61, n, 0);
    g_rand_ready = 0;
    int bad = g_npushed != (int)n;
    for (uint32_t i = 0; i < n && !bad; i++) if (g_pushed[i] != ref[i]) bad = 1;
    if (bad && g_ob_fail < 5)
        printf("FAIL output buffer: pushed %d of %u words (rand_ready=%d)\n", g_npushed, n, rand_ready);
    g_ob_fail += bad; g_ob_runs++;
}

// ---- edge setup (0x62, 0x64..0x67) -------------------------------------------
// reference: geom_triangle.c's edges_of() + geom_triangle_setup_e()'s integer
// part, and geom_fixed.c's fx_recip(), verbatim (int32 wrap spelled out)
static int32_t fx_recip_ref(int32_t a) {
    if (a == 0) return 0x7FFFFFFF;
    uint32_t ua = (a < 0) ? (uint32_t)(0u - (uint32_t)a) : (uint32_t)a;
    uint32_t qmag;
    if (ua <= 1u) {
        qmag = 0xFFFFFFFFu;
    } else {
        uint32_t x = ua; int k = 0;
        while (x < (1u << 17)) { x <<= 1; k++; }
        fx_recip_norm_t kn = fx_recip_norm(x);
        int sh = kn.e + 16 - k;
        qmag = sh ? (kn.r + (1u << (sh - 1))) >> sh : kn.r;
    }
    if (a > 0) return (qmag > 0x7FFFFFFFu) ? 0x7FFFFFFF : (int32_t)qmag;
    return (qmag > 0x80000000u) ? (int32_t)0x80000000 : (int32_t)(0u - qmag);
}
static int32_t to_edge_ref(int32_t v) {
    uint32_t a = v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
    int32_t r = (int32_t)((a + 1024u) >> 11);
    return v < 0 ? -r : r;
}
static int32_t iabs_ref(int32_t v) { return v < 0 ? -v : v; }
static int32_t edge_fn_ref(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t cx, int32_t cy) {
    return (int32_t)((uint32_t)(cx - ax) * (uint32_t)(by - ay) - (uint32_t)(cy - ay) * (uint32_t)(bx - ax));
}
struct EdgeRef { int reject, ok; uint32_t word[12]; int32_t W[6]; };
static void edge_ref(const int32_t *xy, int cull, int grow, const int32_t *sc, EdgeRef *o) {
    int32_t v[6];
    for (int i = 0; i < 6; i++) v[i] = to_edge_ref(xy[i]);
    o->reject = 1; o->ok = 0;
    for (int i = 0; i < 6; i++) if (iabs_ref(v[i]) > 23170) return;
    int32_t area = edge_fn_ref(v[0], v[1], v[2], v[3], v[4], v[5]);
    int32_t sign = cull ? 1 : ((area <= 0) ? -1 : 1);
    area = (int32_t)((uint32_t)area * (uint32_t)sign);
    if (area <= 0) return;
    o->reject = 0;
    const int32_t v0x = v[0], v0y = v[1], v1x = v[2], v1y = v[3], v2x = v[4], v2y = v[5];
    auto mn3 = [](int32_t a, int32_t b, int32_t c) { int32_t m = a < b ? a : b; return m < c ? m : c; };
    auto mx3 = [](int32_t a, int32_t b, int32_t c) { int32_t m = a > b ? a : b; return m > c ? m : c; };
    int32_t bbSX = mn3(v0x, v1x, v2x) + 16, bbSY = mn3(v0y, v1y, v2y) + 16;
    int32_t bbEX = mx3(v0x, v1x, v2x) + 48, bbEY = mx3(v0y, v1y, v2y) + 48;
    int32_t bbSXc = bbSX > sc[0] ? bbSX : sc[0];
    int32_t bbSYc = bbSY > sc[1] ? bbSY : sc[1];
    int32_t bbEXc = bbEX < sc[2] ? bbEX : sc[2];
    int32_t bbEYc = bbEY < sc[3] ? bbEY : sc[3];
    if (bbSXc >= bbEXc || bbSYc >= bbEYc) return;
    o->ok = 1;
    uint16_t sX = (uint16_t)(bbSXc >> 5), sY = (uint16_t)(bbSYc >> 5);
    uint16_t eX = (uint16_t)(bbEXc >> 5), eY = (uint16_t)(bbEYc >> 5);
    o->word[1] = sX | (uint32_t)sY << 16; o->word[2] = eX | (uint32_t)eY << 16;
    int32_t anchorX = (int32_t)sX << 5, anchorY = (int32_t)sY << 5;
    int32_t wi[3], wx[3], wy[3];
    wi[0] = (int32_t)((uint32_t)edge_fn_ref(v1x, v1y, v2x, v2y, anchorX, anchorY) * (uint32_t)sign);
    wi[1] = (int32_t)((uint32_t)edge_fn_ref(v2x, v2y, v0x, v0y, anchorX, anchorY) * (uint32_t)sign);
    wi[2] = (int32_t)((uint32_t)edge_fn_ref(v0x, v0y, v1x, v1y, anchorX, anchorY) * (uint32_t)sign);
    wx[0] = (v2y - v1y) * 32 * sign; wx[1] = (v0y - v2y) * 32 * sign; wx[2] = (v1y - v0y) * 32 * sign;
    wy[0] = (v1x - v2x) * 32 * sign; wy[1] = (v2x - v0x) * 32 * sign; wy[2] = (v0x - v1x) * 32 * sign;
    int32_t wN[3], wXN[3], wYN[3];
    if (area >= (1 << 17)) {
        fx_recip_norm_t k = fx_recip_norm((uint32_t)area);
        for (int j = 0; j < 3; j++) { wN[j] = fx_div_norm(wi[j], k); wXN[j] = fx_div_norm(wx[j], k); wYN[j] = fx_div_norm(wy[j], k); }
    } else {
        int32_t inv = fx_recip_ref(area);
        for (int j = 0; j < 3; j++) { wN[j] = fx_mul(wi[j], inv); wXN[j] = fx_mul(wx[j], inv); wYN[j] = fx_mul(wy[j], inv); }
    }
    for (int j = 0; j < 3; j++)
        if (wx[j] < 0 || (wx[j] == 0 && wy[j] < 0)) wi[j] -= 1;
    if (grow) {  // geom_pipeline.c's GEOM_CRACK_FIX, applied after the weights
        int64_t a2 = (int64_t)wx[0] * wy[1] - (int64_t)wy[0] * wx[1];   /* 1024 * 2 * area */
        if (a2 < 0) a2 = -a2;
        for (int j = 0; j < 3; j++) {
            uint32_t aa = wx[j] < 0 ? 0u - (uint32_t)wx[j] : (uint32_t)wx[j];
            uint32_t bb = wy[j] < 0 ? 0u - (uint32_t)wy[j] : (uint32_t)wy[j];
            if (a2 / 1024 < (int64_t)(aa + bb)) continue;                  /* thinner than ~1 px */
            wi[j] = (int32_t)((uint32_t)wi[j] + ((aa + bb) >> (grow + 1)));   /* level L: >> (L + 1) */
        }
    }
    for (int j = 0; j < 3; j++) { o->word[3 + j] = (uint32_t)wi[j]; o->word[6 + j] = (uint32_t)wx[j]; o->word[9 + j] = (uint32_t)wy[j]; }
    const int32_t W[6] = { wN[1], wN[2], wXN[1], wXN[2], wYN[1], wYN[2] };
    for (int k = 0; k < 6; k++) o->W[k] = sat_bits(W[k], 27);   // stored at 27 bits
}
static int g_edge_fail = 0, g_edge_n = 0, g_edge_ok = 0, g_edge_small = 0, g_lat_eset = 0;
static int32_t g_sc[4]; static int g_cull = -1, g_grow = -1, g_grow_n = 0;
static void test_edges(const int32_t *xy, int cull, const int32_t *sc) {
    if (cull != g_cull) { cfu(0x62, (uint32_t)cull, 4); g_cull = cull; }
    int grow = (int)(rnd32() % 4);   /* 0 off, 1..3: up to 1/4, 1/8, 1/16 px */
    if (grow != g_grow) { cfu(0x62, (uint32_t)grow, 5); g_grow = grow; }
    g_grow_n += grow != 0;
    for (int i = 0; i < 4; i++) if (sc[i] != g_sc[i]) { cfu(0x62, (uint32_t)sc[i], i); g_sc[i] = sc[i]; }
    EdgeRef r; edge_ref(xy, cull, grow, sc, &r);
    cfu(0x64, (uint32_t)xy[0], (uint32_t)xy[1]);
    cfu(0x65, (uint32_t)xy[2], (uint32_t)xy[3]);
    uint32_t rej = cfu(0x66, (uint32_t)xy[4], (uint32_t)xy[5]);
    g_edge_n++;
    int bad = (int)rej != r.reject;
    const char *what = "verdict";
    if (!bad && !r.reject) {
        int lat; uint32_t ok = cfu(0x67, 0, 0, &lat);
        if (lat > g_lat_eset) g_lat_eset = lat;
        if ((int)ok != r.ok) bad = 1;
        else if (ok) {
            g_edge_ok++;
            g_npushed = 0; cfu(0x61, 15, 0);
            for (int k = 1; k < 12 && !bad; k++) if (g_pushed[3 + k] != r.word[k]) { bad = 1; what = "word"; }
            // W through the 3-term blend with one source = 4 and s = 2: exact
            // W through a 2-term blend with a0 = 0, one difference = 4, s = 2:
            // exactly W (4*W / 4, and the sum's fits)
            int32_t hw[6];
            const uint32_t D = 2u | 40u << 8 | 41u << 16 | 42u << 24;   // deposit at 40..42
            cfu(0x40, 4, 0); hw[0] = (int32_t)cfu(0x46, 0, D); hw[2] = (int32_t)cfu(0x5A, 0, 41); hw[4] = (int32_t)cfu(0x5A, 0, 42);
            cfu(0x40, 0, 4); hw[1] = (int32_t)cfu(0x46, 0, D); hw[3] = (int32_t)cfu(0x5A, 0, 41); hw[5] = (int32_t)cfu(0x5A, 0, 42);
            for (int k = 0; k < 6 && !bad; k++) if (hw[k] != r.W[k]) { bad = 1; what = "W"; }
        } else what = "empty box";
    }
    if (bad && g_edge_fail < 8) {
        printf("FAIL edges (%s): xy %d,%d %d,%d %d,%d cull %d grow %d sc %d,%d,%d,%d rej hw %u ref %d\n", what,
               xy[0], xy[1], xy[2], xy[3], xy[4], xy[5], cull, grow, sc[0], sc[1], sc[2], sc[3], rej, r.reject);
    }
    g_edge_fail += bad;
}
static int32_t rnd_px(int lo, int hi) {   // S15.16 screen coordinate in [lo, hi) px, any fraction
    return (int32_t)(((int64_t)lo << 16) + (int64_t)(rnd32() % (uint32_t)((hi - lo) << 16)));
}
static void run_edge_tests() {
    const int32_t sc0[4] = {0, 0, 320 << 5, 240 << 5};
    int32_t xy[6];
    // edge cases: exact rounding ties, guard boundary, int32 extremes, degenerate
    const int32_t special[] = {0, 1024, -1024, 1023, -1023, 3072, -3072, 23170 * 2048 + 1023, 23170 * 2048 + 1024,
                               -(23170 * 2048 + 1023), -(23170 * 2048 + 1024), 1 << 26, -(1 << 26), (1 << 26) + 5,
                               1 << 27, 0x7FFFFFFF, (int32_t)0x80000000, 100 << 16, 200 << 16, -(5 << 16)};
    const int ns = sizeof special / sizeof special[0];
    for (int n = 0; n < 6000; n++) {
        for (int i = 0; i < 6; i++) xy[i] = (rnd32() & 1) ? special[rnd32() % ns] : rnd_px(-40, 360);
        test_edges(xy, n & 1, sc0);
    }
    // tiny areas in edge units (1, 2, 3, ...): fx_recip()'s saturating and
    // unshifted branches
    int hist[4] = {0, 0, 0, 0};
    for (int n = 0; n < 20000; n++) {
        int32_t ox = (int32_t)(rnd32() % 300) * 32, oy = (int32_t)(rnd32() % 200) * 32;
        for (int i = 0; i < 6; i += 2) {
            xy[i] = (ox + (int32_t)(rnd32() % 4)) * 2048 + (int32_t)(rnd32() % 2048) - 1023;
            xy[i + 1] = (oy + (int32_t)(rnd32() % 4)) * 2048 + (int32_t)(rnd32() % 2048) - 1023;
        }
        int32_t e[6]; for (int i = 0; i < 6; i++) e[i] = to_edge_ref(xy[i]);
        int32_t a = edge_fn_ref(e[0], e[1], e[2], e[3], e[4], e[5]); if (a < 0) a = -a;
        if (a < 4) hist[a]++;
        test_edges(xy, n & 1, sc0);
    }
    printf("     tiny areas 0/1/2/3: %d/%d/%d/%d\n", hist[0], hist[1], hist[2], hist[3]);
    for (int n = 0; n < 60000; n++) {
        int kind = n % 6;
        if (kind < 3) {           // on and around the screen
            for (int i = 0; i < 6; i += 2) { xy[i] = rnd_px(-60, 380); xy[i + 1] = rnd_px(-60, 300); }
        } else if (kind < 5) {    // small: a few px around a point (areas < 2^17)
            int32_t cx = rnd_px(-10, 330), cy = rnd_px(-10, 250);
            int span = 1 + (int)(rnd32() % (kind == 3 ? 3 : 12));
            for (int i = 0; i < 6; i += 2) { xy[i] = cx + rnd_px(-span, span); xy[i + 1] = cy + rnd_px(-span, span); }
            if (rnd32() % 8 == 0) { xy[4] = xy[0]; xy[5] = xy[1]; }   // degenerate
        } else {                  // near the guard band
            for (int i = 0; i < 6; i++) xy[i] = rnd_px(-760, 760);
        }
        int32_t sc[4];
        if (n % 4 == 0) {
            // the unit's scissor always starts at 0 (so does the geom pipeline's)
            sc[0] = 0; sc[1] = 0;
            sc[2] = (1 + (int32_t)(rnd32() % 2000)) << 5; sc[3] = (1 + (int32_t)(rnd32() % 2000)) << 5;
            if (n % 8 == 0) { sc[2] -= rnd32() % 32; sc[3] -= rnd32() % 32; }   // not pixel-aligned
        } else memcpy(sc, sc0, sizeof sc);
        test_edges(xy, (rnd32() % 3) != 0, sc);
    }
    printf("%s edge setup: %d of %d triangles mismatched (%d set up); SETUP latency %d cycles\n",
           g_edge_fail ? "FAIL" : "ok  ", g_edge_fail, g_edge_n, g_edge_ok, g_lat_eset);
    printf("     crack grow on in %d of them\n", g_grow_n);
    g_fail += g_edge_fail;
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    dut = new VVpu4DFixed;
    dut->resetn = 0; dut->cmd_valid = 0; dut->rsp_ready = 1;
    for (int i = 0; i < 4; i++) tick();
    dut->resetn = 1; tick();

    run_edge_tests();
    for (int r = 0; r < 400; r++) test_output_buffer(r & 1);
    printf("%s output buffer: %d of %d runs mismatched (DWR, 0x46/0x47 deposits, PUSH with random out_ready)\n",
           g_ob_fail ? "FAIL" : "ok  ", g_ob_fail, g_ob_runs);

    int lat_b = 0, lat_r = 0, lat;
    // edge cases: zero, exact powers, saturation, half-way rounding
    const int32_t Z[6] = {0, 0, 0, 0, 0, 0};
    test_blend(Z, 0, 0, 0, &lat_b);
    test_blend(Z, 1, 5, -7, &lat);
    test_blend(Z, -1, 0, 0, &lat);
    test_blend(Z, 0x7FFFFFFF, 0, 0, &lat);                       // (a0<<16) saturates
    test_blend(Z, (int32_t)0x80000000, 0, 0, &lat);
    const int32_t Wbig[6] = {0x7FFFFFFF, (int32_t)0x80000000, 0x7FFFFFFF, 0x7FFFFFFF,
                             (int32_t)0x80000000, (int32_t)0x80000000};
    test_blend(Wbig, 0, 0x7FFFFFFF, (int32_t)0x80000000, &lat);  // d wraps, huge products
    test_blend(Wbig, 12345, -9999, 424242, &lat);
    const int32_t Wone[6] = {65536, 0, 0, 65536, 32768, 32768};
    for (int k = 0; k < 64; k++) test_blend(Wone, k, k * 3 + 1, -k * 5, &lat);
    // exact rounding boundaries: products whose low bits sit at half an ULP
    for (int sh = 0; sh < 47; sh++) {
        int32_t Wh[6] = {1, 0, 1, 0, -1, 0};
        int64_t p = ((int64_t)1 << sh) + ((sh > 24) ? ((int64_t)1 << (sh - 24)) : 0);
        int32_t d = (int32_t)(p > 0x7FFFFFFF ? 0x7FFFFFFF : p);
        test_blend(Wh, 0, d, 0, &lat);
        test_blend(Wh, 0, d + 1, 0, &lat);
        test_blend(Wh, 0, -d, 0, &lat);
    }
    // random: realistic (attributes S15.16 up to 2^24, weights up to 2^20) and wild
    for (int n = 0; n < 40000; n++) {
        int wild = (n % 4) == 0;
        int32_t W[6];
        for (int i = 0; i < 6; i++) W[i] = wild ? (int32_t)rnd32() : rnd_range(20);
        int32_t a0 = wild ? (int32_t)rnd32() : rnd_range(24);
        int32_t a1 = wild ? (int32_t)rnd32() : rnd_range(24);
        int32_t a2 = wild ? (int32_t)rnd32() : rnd_range(24);
        test_blend(W, a0, a1, a2, &lat);
    }

    for (int n = 0; n < 5000; n++) {
        uint32_t a = (rnd32() >> (rnd32() % 15)) | (1u << 17);
        if (a > 0x80000000u) a >>= 1;
        test_divn_sw(a);
    }
#ifndef SU_NO_RECIP
    // reciprocal: bounds, powers of two and neighbours, random
    test_recip(1u << 17, &lat_r);
    // fx_recip_norm()'s domain is 2^17 <= a <= 2^31 (a triangle area or |w|
    // of an int32); above 2^31 its own `r <<= 1` overflows ("r < a <= 2^31"),
    // so the C is not a valid reference there. The RTL is exact everywhere.
    test_recip(0x80000000u, &lat);
    for (int b = 17; b < 31; b++) {
        test_recip(1u << b, &lat); test_recip((1u << b) + 1, &lat); test_recip((1u << b) - 1 + (b == 17), &lat);
        test_recip((1u << b) | (1u << (b - 1)), &lat);
    }
    for (int n = 0; n < 40000; n++) {
        uint32_t a = rnd32();
        if (a < (1u << 17)) a |= 1u << 17;
        if (n & 1) a >>= (rnd32() % 15);
        if (a < (1u << 17)) a = (1u << 17) + (a & 0xFFFF);
        if (a > 0x80000000u) a >>= 1;
        test_recip(a, &lat);
    }

#endif
    // 2-term blends with the differences and weights at and past their
    // saturation bounds (20 / 27 bits)
    for (int n = 0; n < 30000; n++) {
        int32_t W[6];
        for (int i = 0; i < 6; i++) W[i] = (n & 1) ? rnd_range(28) : (int32_t)rnd32();
        int32_t a0 = rnd_range(n & 2 ? 21 : 31), a1 = rnd_range(n & 4 ? 21 : 31), a2 = rnd_range(21);
        test_blend(W, a0, a1, a2, &lat);
    }
#ifndef SU_NO_NORMF
    // NORMF: every len2 an int8 normal can produce, plus beyond
    for (uint32_t l = 0; l <= 3u * 128u * 128u; l++) test_normf(l);
    for (int n = 0; n < 5000; n++) test_normf(rnd32() & 0xFFFF);
#endif
    printf("normf latency %d cycles\n", g_lat_nf);



    // fx_mul: signs, saturation, exact halves
    const int32_t ev[] = {0, 1, -1, 32768, -32768, 65536, -65536, 0x7FFFFFFF, (int32_t)0x80000000, 3, -3, 98304, -98304};
    for (int32_t x : ev) for (int32_t y : ev) test_mul(x, y);
    for (int n = 0; n < 40000; n++) test_mul((n & 1) ? (int32_t)rnd32() : rnd_range(24), (n & 2) ? (int32_t)rnd32() : rnd_range(20));
    printf("fx_mul latency %d cycles, fx_div_norm latency %d cycles\n", g_lat_mul, g_lat_div);

#ifndef SU_NO_RECIP
    // PROJECT: matrices in +-2^21 (S17.10: +-2048.0), vertices in +-2^20; the
    // MATVEC saturates some outputs, which the column shifts then saturate
    // again -- all of it has to match. Plus w forced tiny, negative and zero.
    for (int n = 0; n < 20000; n++) {
        int32_t M[16], A[4]; int sh[4];
        for (int i = 0; i < 16; i++) M[i] = rnd_range(n % 3 ? 21 : 25);
        for (int i = 0; i < 3; i++) A[i] = rnd_range(n % 2 ? 20 : 24);
        A[3] = 1 << 10;
        for (int j = 0; j < 4; j++) sh[j] = (int)(rnd32() % 7);
        if (n % 7 == 0) { M[3] = M[7] = M[11] = 0; M[15] = (int32_t)(rnd32() % 256) - 128; }   // |w| ~ 0..2^17
        int32_t sx = (n % 5) ? (133 << 16) + rnd_range(12) : (int32_t)rnd32();
        int32_t sy = (n % 5) ? (120 << 16) + rnd_range(12) : (int32_t)rnd32();
        int32_t tx = (n % 5) ? (133 << 16) : (int32_t)rnd32();
        int32_t ty = (n % 5) ? (120 << 16) : (int32_t)rnd32();
        test_project(M, A, sh, sx, tx, sy, ty);
    }
    printf("PROJECT latency %d cycles\n", g_lat_proj);
#endif
    printf("setup unit: %ld vectors, %d mismatches; latency blend-go %d cycles, recip %d cycles\n",
           g_n, g_fail, lat_b, lat_r);
    printf(g_fail ? "FAIL\n" : "PASS\n");
    delete dut;
    return g_fail ? 1 : 0;
}
