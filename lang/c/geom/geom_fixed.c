/* Implementations for geom_fixed.h -- see that file's header comment for the
 * format rationale (S15.16) and why these are real functions, not
 * `static inline` (ROM budget). */
#define GEOM_FIXED_IMPL   /* keep the real fx_mul() definition below */
#include "geom_fixed.h"
#include "geom_vpar.h"

int32_t fx_clamp(int64_t v) {
    if (v > FX_MAX_RAW) return FX_MAX_RAW;
    if (v < FX_MIN_RAW) return FX_MIN_RAW;
    return (int32_t)v;
}

/* ---- core fixed-point arithmetic (native rv32im mul/div, no FPU) ------- */

int32_t fx_mul(int32_t a, int32_t b) {
    int64_t p = (int64_t)a * (int64_t)b;
    int64_t half = 1LL << (FX_FRAC_BITS - 1);
    p = (p >= 0) ? (p + half) >> FX_FRAC_BITS : -(((-p) + half) >> FX_FRAC_BITS);
    return fx_clamp(p);
}
int32_t fx_add(int32_t a, int32_t b) { return fx_clamp((int64_t)a + (int64_t)b); }
int32_t fx_sub(int32_t a, int32_t b) { return fx_clamp((int64_t)a - (int64_t)b); }

/* Reciprocal: 1/a in S15.16 = round(2^32 / |a|), sign-adjusted. 2*FRAC_BITS
 * == 32 doesn't fit an int32 numerator, so a naive port of this would need
 * an int64 dividend -- routed through nolibc.c's __divdi3 (this build is
 * -nostdlib, no libgcc, and rv32im's native DIV is 32-bit only). That
 * shim's own size (and every one of its call sites) was the difference
 * between fitting the geom core's 16KB ROM and not the first time this was
 * built for the real target, so this is deliberately reformulated to use
 * ONLY native 32-bit ops instead:
 *
 * Let ua = |a| (ua >= 2 after the ua <= 1 fast-out below, since those two
 * cases -- 1/1 and 1/-1 -- overflow S15.16's own range regardless of exact
 * rounding and clamp either way). Write 2^32 = ua*Q + R (0 <= R < ua); Q is
 * the unrounded quotient. m = 2^32 - ua is exactly representable as a
 * uint32_t (no overflow, since ua >= 2), and m = ua*(Q-1) + R (substitute
 * and check: ua*(Q-1)+R = ua*Q - ua + R = 2^32 - ua = m, exactly), so a
 * single NATIVE unsigned divide gives q0 = m/ua = Q-1 and r0 = m%ua = R --
 * both exact, both one rv32im DIVU/REMU each.
 *
 * Rounding to nearest adds half = ua/2 before dividing: since R < ua and
 * half < ua, R+half < 2*ua, so the rounded quotient is (Q-1)+1 plus one
 * more if R+half carries past ua -- i.e. qmag = q0 + 1 + (r0+half >= ua).
 * Verified by hand for several cases including an exact round-up (a=7:
 * 2^32/7 = 613566756.57, R=4, half=3, R+half=7 >= 7 -> carries to 613566757,
 * matching round-to-nearest) before ever building it. */
int32_t fx_recip(int32_t a) {
    /* 2^32 / a, i.e. 1/a in S15.16 for a raw S15.16 a, through
     * fx_recip_norm() -- the GeomSetupUnit's RECIPN on the geom core -- with
     * |a| shifted up into its domain and the result shifted back, rounded:
     * <= 2^-19.8 relative, and w = 1.0 still gives exactly 1.0 (the
     * orthographic HUD and skybox). This used to be an exact rv32 DIVU (~35
     * cycles); with it and two trivial divides gone the geom core needs no
     * hardware divider (built rv32i_zmmul). R <= 2^31, so R + half fits. */
    if (a == 0) return FX_MAX_RAW;
    uint32_t ua = (a < 0) ? (uint32_t)(0u - (uint32_t)a) : (uint32_t)a;
    uint32_t qmag;
    if (ua <= 1u) {
        qmag = 0xFFFFFFFFu;   /* 2^32/1 overflows S15.16 regardless -- clamped below */
    } else {
        uint32_t x = ua; int k = 0;
        while (x < (1u << 17)) { x <<= 1; k++; }     /* into [2^17, 2^31] */
        fx_recip_norm_t kn = fx_recip_norm(x);      /* R ~ 2^(32+s) / x */
        int sh = kn.e + 16 - k;                     /* s - k >= 0 */
        qmag = sh ? (kn.r + (1u << (sh - 1))) >> sh : kn.r;
    }
    if (a > 0) return (qmag > (uint32_t)FX_MAX_RAW) ? FX_MAX_RAW : (int32_t)qmag;
    return (qmag > 0x80000000u) ? FX_MIN_RAW : (int32_t)(0u - qmag);
}

/* Plain-integer division num/den -> S15.16 fixed, num and den RAW ints (not
 * already-fixed values) with no range restriction beyond int32 -- unlike
 * fx_from_int()+fx_div(), this never materializes `num` or `den` as a
 * standalone S15.16 value in between, so it's safe for operands far outside
 * +-32767 as long as the QUOTIENT itself fits. Needed by triangle setup's
 * barycentric-weight normalisation (geom_triangle.c): edge-function areas
 * run into the millions (screen coords scaled by EDGE_FUNC_SHIFT), but
 * wInit[k]/area is always a small ratio. Routing that through
 * vpu_itof(area) first (S15.16, integer part maxes at 32767) silently
 * saturated both the area AND every wInit[k] to the same ~32768 cap,
 * collapsing every barycentric weight to +-1.0 regardless of its real value
 * -- found via test_geom's scene 1 (color/depth interpolation went to
 * +-0.999985 for every field, the S15.16 near-1.0 saturation signature).
 *
 * num/den == num * (1/den) -- the same decomposition the real N64 RSP uses
 * for any general division, since it has no divider at all: VRCP looks up
 * an approximate 1/x in a hardware table, VMULF multiplies. fx_recip()
 * already treats its argument as a raw 32-bit integer and computes an
 * EXACT (not table-approximated) S15.16 reciprocal of it using only native
 * 32-bit ops (see its own comment) -- composing fx_mul(num, fx_recip(den))
 * needs no wide-dividend division at all: fx_recip(den) = round(2^32/den),
 * and fx_mul(num, that) = round(num*round(2^32/den)/65536) ==
 * round(num*65536/den) to within one double-rounding step (at most
 * 1/65536), utterly negligible next to this function's own callers'
 * tolerances (0.002-0.01 relative -- see test_geom_fixed.c). fx_mul() only
 * ever needs a 64-bit MULTIPLY, which rv32im's M-extension synthesizes
 * inline (native 32x32->64 MUL/MULH, no libgcc call) -- unlike a 64-bit
 * DIVIDE, which this ISA has no hardware for at all (nolibc.c's __divdi3
 * comment). After this rewrite __divdi3 has no remaining caller anywhere in
 * this firmware, so the linker drops the whole thing. */
int32_t fx_div_ii(int32_t num, int32_t den) {
    if (den == 0) return num >= 0 ? FX_MAX_RAW : FX_MIN_RAW;
    return fx_mul(num, fx_recip(den));
}

/* Weighted sum of 3 fixed values -- the WSUM3 building block
 * (geom_triangle.c's barycentric blends): out = a*w0 + b*w1 + c*w2, rounded
 * once at the end (matching the CFU's own single-round-at-the-end
 * convention, minimizing compounded rounding error). */
int32_t fx_wsum3(int32_t a, int32_t w0, int32_t b, int32_t w1, int32_t c, int32_t w2) {
    int64_t p = (int64_t)a * w0 + (int64_t)b * w1 + (int64_t)c * w2;
    int64_t half = 1LL << (FX_FRAC_BITS - 1);
    p = (p >= 0) ? (p + half) >> FX_FRAC_BITS : -(((-p) + half) >> FX_FRAC_BITS);
    return fx_clamp(p);
}

/* int <-> fixed (exact, trivial shift -- object coordinates are integers). */
int32_t fx_from_int(int32_t i) { return fx_clamp((int64_t)i << FX_FRAC_BITS); }

/* ---- IEEE-754 binary32 bit pattern <-> S15.16, pure integer ------------
 * Needed at every geom-core boundary that carries a real float32: GDL words
 * from the game CPU (which DOES have an FPU). */
/* Generic core, parameterised over fractional bits and clamp range -- also
 * used by geom_vpar.h's S17.10 CFU boundary conversion (same algorithm,
 * different constants; see this function's declaration in geom_fixed.h). */
/* Both conversions sit on EVERY scalar op (each vpu_* decodes its float
 * operands and re-encodes its result), and a cycle-accurate profile of the
 * real geom core (sim/geom_full, 2026-09-22) put them at 64% of all
 * vertex-processing time: 28% here, 36% in fx_to_bits_n. Both are written
 * to stay in 32-bit arithmetic -- an earlier version shifted in int64_t,
 * which on rv32 is a call to __ashldi3 plus 64-bit compares, and found the
 * top set bit with a 31-iteration loop. Results are bit-identical to that
 * version (checked on every exponent/sign and random mantissas for both
 * the S15.16 and S17.10 parameterisations). */
int32_t fx_from_bits_n(uint32_t bits, int frac_bits, int32_t max_raw, int32_t min_raw) {
    uint32_t sign = bits >> 31;
    int32_t  exp  = (int32_t)((bits >> 23) & 0xFFu);
    int32_t  v;
    if (exp == 0) {
        /* zero or denormal: denormals (< ~1.2e-38) are far below either
         * format's step either way -- both round to 0. */
        return 0;
    }
    if (exp == 0xFF) {
        /* inf/nan: not expected on this data path; saturate rather than
         * propagate garbage. */
        return sign ? -max_raw : max_raw;
    }
    uint32_t m24 = 0x800000u | (bits & 0x7FFFFFu);    /* Q24: bit23 = 1.0 */
    int shift = exp - 127 - 23 + frac_bits;           /* value*2^frac_bits = m24 * 2^shift */
    if (shift >= 0) {
        /* m24 >= 2^23, so from shift 8 on the magnitude is >= 2^31: past
         * either format's range whatever the sign. */
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
uint32_t fx_to_bits_n(int32_t fx, int frac_bits) {
    if (fx == 0) return 0;
    uint32_t sign = (fx < 0) ? (1u << 31) : 0;
    uint32_t m = (fx < 0) ? 0u - (uint32_t)fx : (uint32_t)fx;
    /* index of highest set bit (m != 0), binary search -- rv32im has no clz */
    int msb = 0; uint32_t t = m;
    if (t >> 16) { t >>= 16; msb += 16; }
    if (t >>  8) { t >>=  8; msb +=  8; }
    if (t >>  4) { t >>=  4; msb +=  4; }
    if (t >>  2) { t >>=  2; msb +=  2; }
    if (t >>  1) {           msb +=  1; }
    int shift = msb - 23;                             /* bring msb to bit 23 */
    int exp = msb - frac_bits + 127;
    uint32_t mant24;
    if (shift >= 0) {
        uint32_t half = (shift > 0) ? (1u << (shift - 1)) : 0;
        mant24 = (m + half) >> shift;
        /* rounding carried past bit23 (mant24 hit 2^24, i.e. rounded up to
         * the next power of two) -- the value's exponent goes up by one too,
         * not just the mantissa shift. Missing this silently halved any
         * value that rounds up through a power-of-two boundary (e.g.
         * 8191.9997 -> encoded as ~4096) -- caught by test_geom_fixed's
         * fx_add sweep, not a hand-picked case. */
        if (mant24 & 0x1000000u) { mant24 >>= 1; exp++; }
    } else {
        mant24 = m << (-shift);
    }
    if (exp <= 0) return sign;                 /* underflow to zero */
    if (exp >= 255) return sign | (0xFFu << 23);
    return sign | ((uint32_t)exp << 23) | (mant24 & 0x7FFFFFu);
}

int32_t fx_from_bits(uint32_t bits) {
#ifdef GEOM_FX_CONV_CFU
    if (g_su_conv) return fx_from_bits_cfu(bits, 0);
#endif
    return fx_from_bits_n(bits, FX_FRAC_BITS, FX_MAX_RAW, FX_MIN_RAW);
}
uint32_t fx_to_bits(int32_t fx) {
#ifdef GEOM_FX_CONV_CFU
    if (g_su_conv) return fx_to_bits_cfu(fx);
#endif
    return fx_to_bits_n(fx, FX_FRAC_BITS);
}
#ifdef GEOM_FX_CONV_CFU
int32_t geom_vpar_to_fixed_bits(uint32_t bits) {
    if (g_su_conv) return fx_from_bits_cfu(bits, 1);
    return fx_from_bits_n(bits, 10, (1 << 26) - 1, -(1 << 26));   /* == GEOM_VPAR_* */
}
#endif

/* Normalised reciprocal for high-precision division by a positive raw int
 * (see geom_fixed.h): R ~ 2^(32+s)/a with s = max(msb(a) - 9, 16), and
 * x/a*2^16 = high word of x*R shifted by e = s-16. Used for the barycentric
 * weights (x / triangle area) and the perspective divide (x / w), where
 * fx_recip()'s 2^32/a keeps only ~6 bits once w is in the thousands.
 *
 * R is a table seed refined by one Newton step, the N64 RSP's way (its VRCP
 * is a 512-entry table indexed by the 9 bits after the leading one):
 * relative error <= 2^-19.8 over the whole
 * domain, ~0.0003 px on screen. It replaced an exact long division that cost
 * the geom core's CFU 53 cycles a call, one call per vertex and per triangle
 * (11.5% of a game frame with the divides that use it). This C and
 * rtl/vpu/GeomSetupUnit.v's 0x50 are the same integer steps and must stay
 * bit-identical (sim/vpu/tb_setupunit.cpp). The table: tools/gen_rcp_tab.py. */
#include "geom_rcp_tab.h"
fx_recip_norm_t fx_recip_norm(uint32_t a)            /* a >= FX_RECIP_NORM_MIN */
{
#ifdef GEOM_SETUP_HW
    if (g_su_recip) {
        fx_recip_norm_t kh = { geom_setup_recipn(a), 0 };
        kh.e = geom_setup_recipn_e();
        return kh;
    }
#endif
    int msb = 0; uint32_t t = a;
    if (t >> 16) { t >>= 16; msb += 16; }
    if (t >>  8) { t >>=  8; msb +=  8; }
    if (t >>  4) { t >>=  4; msb +=  4; }
    if (t >>  2) { t >>=  2; msb +=  2; }
    if (t >>  1) {           msb +=  1; }
    int s = msb - 9;
    if (s < 16) s = 16;                                 /* a >= 2^17: R still < 2^32 */
    uint32_t m   = a << (31 - msb);                     /* [2^31, 2^32) */
    int32_t  r0  = (int32_t)((0x10000u | geom_rcp_tab[(m >> 22) & 511u]) << 6);  /* ~2^23/m' */
    uint64_t e46 = (uint64_t)(m >> 8) * (uint32_t)r0;   /* ~2^46 */
    int64_t  eps = (int64_t)(1ull << 46) - (int64_t)e46;
    int32_t  r1  = r0 + (int32_t)(((int64_t)r0 * (eps >> 10)) >> 36);
    fx_recip_norm_t k = { (uint32_t)r1 << (9 + s - msb), s - 16 };
#ifdef GEOM_FX_DIVN_LAST
    { int32_t rd_; GEOM_FX_CFU(0x57, rd_, k.e, 0); (void)rd_; }   /* e for fx_div_norm_last() */
#endif
    return k;
}

#if defined(GEOM_SETUP_HW) && defined(GEOM_SU_PROBE)
uint8_t g_su_recip, g_su_normf, g_su_conv;

/* Which optional GeomSetupUnit ops this bitstream has: each is asked one
 * question whose answer the C knows (an absent op answers 0). Run before
 * anything converts, divides or lights -- geom_reset() calls it first. */
void geom_setup_probe(void)
{
    g_su_recip = g_su_normf = g_su_conv = 0;
    fx_recip_norm_t k = fx_recip_norm(131072u + 12345u);         /* the C, flags still 0 */
    g_su_recip = geom_setup_recipn(131072u + 12345u) == k.r;
    g_su_normf = geom_setup_normf(3) == (1u << 24) / 27u;       /* isqrt(3 << 8) = 27 */
    g_su_conv  = fx_from_bits_cfu(0x3FC00000u, 0) == 98304;       /* 1.5f in S15.16 */
    if (!g_su_recip) (void)fx_recip_norm(131072u);                /* leave a valid e behind */
}
#endif

/* round(x * R / 2^(32+e)), halves rounded up. |x*R| < 2^62, so the high
 * word (< 2^30) never overflows. */
int32_t fx_div_norm(int32_t x, fx_recip_norm_t k)
{
    int64_t p = (int64_t)x * (int64_t)k.r;
    int32_t hi = (int32_t)(p >> 32);
    if (k.e == 0) return hi + (int32_t)((uint32_t)p >> 31);
    return (hi >> k.e) + ((hi >> (k.e - 1)) & 1);
}

