/* The audio core's DSP instructions (rtl/audio/AudioDspCfu.v), as intrinsics
 * on the audio core and as a C model everywhere else. The model is what
 * sim/audio's tb_cfu checks the RTL against -- keep the two in step.
 *
 * Every op is exact only inside its range, which the caller checks:
 *   envelope mixer: lane values 0..2^31-1, target 0..32767, rate 0..0x1FFFF;
 *   resampler:      step (pitch << 1) 0..0x1FFFF;
 *   ADPCM decoder:  table index 0..7.
 */
#ifndef AUDIO_CFU_H
#define AUDIO_CFU_H

#include <stdint.h>

#define ACFU_ENV_CFG_A 0x01   /* in0 = rate | channel << 31, in1 = target      */
#define ACFU_ENV_CFG_B 0x02   /* in0 = vol_dry, in1 = vol_wet                   */
#define ACFU_ENV_VSET  0x03   /* in0 = lane (channel * 8 + i), in1 = volume     */
#define ACFU_ENV_VGET  0x04   /* in0 = lane -> volume                            */
#define ACFU_ENV_START 0x05   /* in0 = aux; cursor to channel 0, pair 0, dry     */
#define ACFU_ENV_MIX2  0x06   /* in0 = {o1, o0}, in1 = {x1, x0} -> {o1', o0'}    */
#define ACFU_RS_CFG    0x08
#define ACFU_RS_ACC    0x09
#define ACFU_RS_OUT    0x0A
#define ACFU_ADP_TLOAD 0x07   /* in0 = {c1, c0}, in1 = {c3, c2} -> codebook word tptr++ */
#define ACFU_ADP_TRST  0x0E   /* tptr = in0 (0..31)                               */
#define ACFU_ADP_START 0x0B   /* in0 = {out[-1], out[-2]}                          */
#define ACFU_ADP_GRP   0x0C   /* in0 = frame header, in1 = 4 data bytes (first in
                                 [31:24]) -> {out1, out0}                          */
#define ACFU_ADP_NXT   0x0D   /* -> {out3, out2}, {out5, out4}, {out7, out6}       */
#define ACFU_ID        0x0F
#define ACFU_ID_VALUE  0x41445333u   /* "ADS3" */
#define ACFU_ID_ADS2   0x41445332u   /* the build without the ADPCM ops */

#if defined(__riscv) && !defined(ACFU_MODEL)
/* custom-0, funct3 = 0, funct7 = id (VexRiscv delivers {funct3, funct7}) */
#define ACFU(id, a, b) ({ uint32_t _r; \
    __asm__ volatile(".insn r 0x0B, 0, %3, %0, %1, %2" : "=r"(_r) : "r"((uint32_t)(a)), "r"((uint32_t)(b)), "i"(id)); \
    _r; })
#else
/* ---- C model (bit-exact with the RTL) ---- */
extern const int16_t acfu_resample_table[64][4];
static struct {
    uint32_t rate[2]; int32_t target[2], vol_d, vol_w;
    uint32_t vols[16]; int aux, cc, pp, ss;
    uint32_t step, acc;
    int16_t book[32][4]; uint32_t tptr;        /* ADPCM codebook, flat [8][2][8] */
    int16_t prev1, prev2, hist[8]; uint32_t nibs, shift, tidx, j;
} acfu_s;
static inline int16_t acfu_clamp16(int64_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
/* one ADPCM output: out_j from the history, then x_j shifts into it */
static inline int16_t acfu_adp_one(void)
{
    uint32_t j = acfu_s.j & 7;
    int32_t nib = (int32_t)(acfu_s.nibs << (4 * j)) >> 28;
    int16_t x = (int16_t)((uint32_t)nib << acfu_s.shift);
    const int16_t *c0 = acfu_s.book[acfu_s.tidx * 4 + (j >> 2)];
    uint32_t a = (uint32_t)((int32_t)c0[j & 3] * acfu_s.prev2) + ((uint32_t)(int32_t)x << 11);
    for (int m = 0; m < 8; m++)
        a += (uint32_t)((int32_t)acfu_s.book[acfu_s.tidx * 4 + 2 + (m >> 2)][m & 3] * acfu_s.hist[m]);
    for (int m = 7; m > 0; m--) acfu_s.hist[m] = acfu_s.hist[m - 1];
    acfu_s.hist[0] = x;
    acfu_s.j = j + 1;
    return acfu_clamp16((int32_t)a >> 11);
}
static inline uint32_t acfu_adp_pair(void)
{
    int16_t lo = acfu_adp_one(), hi = acfu_adp_one();
    if (acfu_s.j == 8) { acfu_s.prev2 = lo; acfu_s.prev1 = hi; }
    return (uint32_t)(uint16_t)lo | (uint32_t)(uint16_t)hi << 16;
}
static inline uint32_t acfu_model(uint32_t id, uint32_t a, uint32_t b)
{
    switch (id) {
    case ACFU_ENV_CFG_A: acfu_s.rate[a >> 31] = a & 0x1FFFF; acfu_s.target[a >> 31] = (int32_t)(b & 0xFFFF); return 0;
    case ACFU_ENV_CFG_B: acfu_s.vol_d = (int16_t)a; acfu_s.vol_w = (int16_t)b; return 0;
    case ACFU_ENV_VSET: acfu_s.vols[a & 15] = b & 0x7FFFFFFF; return 0;
    case ACFU_ENV_VGET: return acfu_s.vols[a & 15];
    case ACFU_ENV_START: acfu_s.aux = a & 1; acfu_s.cc = acfu_s.pp = acfu_s.ss = 0; return 0;
    case ACFU_ENV_MIX2: {
        int c = acfu_s.cc, last = !acfu_s.aux || acfu_s.ss;
        uint32_t rate = acfu_s.rate[c], r = 0;
        int32_t vol = acfu_s.ss ? acfu_s.vol_w : acfu_s.vol_d;
        for (int k = 0; k < 2; k++) {
            uint32_t *vp = &acfu_s.vols[c * 8 + acfu_s.pp * 2 + k], v = *vp, hi = v >> 16;
            if ((rate >> 16) & 1 ? (int32_t)hi > acfu_s.target[c] : (int32_t)hi < acfu_s.target[c])
                v = (uint32_t)(acfu_s.target[c] & 0x7FFF) << 16;
            int32_t g = ((int32_t)(v >> 16) * vol + 0x4000) >> 15;
            int32_t o = (int16_t)((k ? a >> 16 : a) & 0xFFFF), x = (int16_t)((k ? b >> 16 : b) & 0xFFFF);
            r |= (uint32_t)(uint16_t)acfu_clamp16((((int64_t)o << 15) - o + (int64_t)x * g + 0x4000) >> 15) << (16 * k);
            if (last) {
                uint64_t n = (uint64_t)(v >> 16) * rate + (((uint64_t)(v & 0xFFFF) * rate) >> 16);
                *vp = n > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)n;
            }
        }
        if (acfu_s.aux && !acfu_s.ss) acfu_s.ss = 1;
        else { acfu_s.ss = 0; if (++acfu_s.pp == 4) { acfu_s.pp = 0; acfu_s.cc ^= 1; } }
        return r;
    }
    case ACFU_RS_CFG: acfu_s.step = a & 0x1FFFF; acfu_s.acc = b & 0xFFFF; return 0;
    case ACFU_RS_ACC: return acfu_s.acc;
    case ACFU_RS_OUT: {
        const int16_t *t = acfu_resample_table[acfu_s.acc >> 10];
        int16_t sm[4] = { (int16_t)a, (int16_t)(a >> 16), (int16_t)b, (int16_t)(b >> 16) };
        int32_t y = 0;
        for (int j = 0; j < 4; j++) y += (sm[j] * t[j] + 0x4000) >> 15;
        uint32_t n = acfu_s.acc + acfu_s.step;
        acfu_s.acc = n & 0xFFFF;
        return ((n >> 16) << 16) | (uint16_t)acfu_clamp16(y);
    }
    case ACFU_ADP_TLOAD: {
        int16_t *w = acfu_s.book[acfu_s.tptr++ & 31];
        w[0] = (int16_t)a; w[1] = (int16_t)(a >> 16); w[2] = (int16_t)b; w[3] = (int16_t)(b >> 16);
        return 0;
    }
    case ACFU_ADP_TRST: acfu_s.tptr = a & 31; return 0;
    case ACFU_ADP_START: acfu_s.prev2 = (int16_t)a; acfu_s.prev1 = (int16_t)(a >> 16); return 0;
    case ACFU_ADP_GRP:
        acfu_s.shift = (a >> 4) & 15; acfu_s.tidx = a & 7; acfu_s.nibs = b; acfu_s.j = 0;
        for (int m = 1; m < 8; m++) acfu_s.hist[m] = 0;
        acfu_s.hist[0] = acfu_s.prev1;
        return acfu_adp_pair();
    case ACFU_ADP_NXT: return acfu_adp_pair();
    case ACFU_ID: return ACFU_ID_VALUE;
    default: return 0;
    }
}
#define ACFU(id, a, b) acfu_model((id), (uint32_t)(a), (uint32_t)(b))
#endif

#endif
