/* Audio core bring-up program. Checks what the core needs before
 * any real audio code runs on it, measures it, then plays a tone:
 *
 *   T0 multiply (Zmmul: mul/mulh/mulhu)      T3 APF audio FIFO CSRs
 *   T1 byte/half loads and stores in DMEM    T4 the CTRL cycle counter
 *   T2 SDRAM word/byte access through the Wishbone master
 *   T5 the DSP instructions (AudioDspCfu): ID, and one resampler output
 *
 * Mailbox (AUDIO_MBOX(i), see lang/c/game/tone_main.c for the other side):
 *   in : MBOX1 = SDRAM scratch address (set by the game CPU before RUN)
 *   out: MBOX0 = state, MBOX2 = pass bits, MBOX3 = fail bits,
 *        MBOX4 = samples pushed to the FIFO so far,
 *        MBOX5..7 = cycles: 16-tap FIR x256, envelope mix x256,
 *                   256 SDRAM word loads.
 * Then it keeps the APF FIFO topped up with a 440 Hz sine, forever.
 */
#include <stdint.h>
#include "audio_hw.h"
#include "generated_csr_addrs.h"
#include "audio_cfu.h"

#define ST_TESTING 0xA0D10001u
#define ST_TONE    0xA0D10002u

static inline void     w32(uint32_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t r32(uint32_t a)             { return *(volatile uint32_t *)a; }
static inline uint32_t cycles(void)                { return r32(AUDIO_CYCLES); }

static uint32_t s_pass, s_fail;
static void result(int t, int ok) { if (ok) s_pass |= 1u << t; else s_fail |= 1u << t; }

/* Inputs the compiler cannot fold. */
static volatile int32_t  v_a = -123456, v_b = 789;
static volatile uint32_t v_c = 0x7fffffffu, v_d = 0xdeadbeefu, v_e = 0xcafebabeu;

static int test_mul(void)
{
    int32_t a = v_a, b = v_b;
    uint32_t c = v_c, d = v_d, e = v_e;
    int ok = 1;
    ok &= (uint32_t)(a * b) == 4197560512u;
    ok &= (uint32_t)(((int64_t)a * b) >> 32) == 4294967295u;
    ok &= (uint32_t)(((uint64_t)(uint32_t)a * c) >> 32) == 2147421919u;
    ok &= (uint32_t)(((uint64_t)d * e) >> 32) == 2962402171u;
    ok &= d * e == 2295290722u;
    return ok;
}

static volatile uint32_t v_word;
static int test_subword(void)
{
    volatile uint8_t  *b = (volatile uint8_t *)&v_word;
    volatile uint16_t *h = (volatile uint16_t *)&v_word;
    int ok = 1;
    v_word = 0;
    b[1] = 0x80;
    h[1] = 0xfffe;
    ok &= v_word == 0xfffe8000u;
    ok &= ((volatile int8_t *)b)[1] == -128;
    ok &= b[1] == 0x80;
    ok &= ((volatile int16_t *)h)[0] == (int16_t)0x8000;
    ok &= h[1] == 0xfffe;
    b[0] = 0x12; b[3] = 0x34;
    ok &= v_word == 0x34fe8012u;
    return ok;
}

static int test_sdram(uint32_t base)
{
    volatile uint32_t *p = (volatile uint32_t *)base;
    int ok = 1;
    for (uint32_t i = 0; i < 64; i++) p[i] = i * 0x9E3779B9u;
    for (uint32_t i = 0; i < 64; i++) ok &= p[i] == i * 0x9E3779B9u;
    /* sub-word stores must only touch their own byte lanes */
    p[64] = 0x11223344u;
    ((volatile uint8_t *)&p[64])[2] = 0xAB;
    ((volatile uint16_t *)&p[64])[0] = 0xCDEF;
    ok &= p[64] == 0x11ABCDEFu;
    ok &= ((volatile int8_t *)&p[64])[2] == (int8_t)0xAB;
    return ok;
}

static void spin(uint32_t n) { uint32_t t = cycles(); while (cycles() - t < n) { } }

static int test_fifo(void)
{
    int ok = 1;
    w32(CSR_APF_AUDIO_PLAYBACK_EN_ADDR, 0);
    w32(CSR_APF_AUDIO_BUFFER_FLUSH_ADDR, 1);
    spin(64);
    ok &= r32(CSR_APF_AUDIO_BUFFER_FILL_ADDR) == 0;
    for (int i = 0; i < 10; i++) w32(CSR_APF_AUDIO_OUT_ADDR, 0);
    spin(64);
    ok &= r32(CSR_APF_AUDIO_BUFFER_FILL_ADDR) == 10;
    w32(CSR_APF_AUDIO_BUFFER_FLUSH_ADDR, 1);
    spin(64);
    ok &= r32(CSR_APF_AUDIO_BUFFER_FILL_ADDR) == 0;
    return ok;
}

static int test_cycles(void)
{
    uint32_t t0 = cycles();
    for (volatile int i = 0; i < 1000; i++) { }
    uint32_t dt = cycles() - t0;
    return dt >= 1000u && dt < 40000u;
}

/* ---------------------------------------------------------- kernels */
static int16_t k_in[256 + 16], k_coef[16], k_out[256], k_l[256], k_r[256];

static inline int16_t sat16(int32_t v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

/* 16-tap FIR, the shape of the ADPCM predictor / resampler inner loops. */
static uint32_t bench_fir(void)
{
    uint32_t t0 = cycles();
    for (int n = 0; n < 256; n++) {
        int32_t acc = 0;
        for (int k = 0; k < 16; k++) acc += k_in[n + k] * k_coef[k];
        k_out[n] = sat16(acc >> 15);
    }
    return cycles() - t0;
}

/* Envelope mixer: one voice into a stereo pair with a volume ramp. */
static uint32_t bench_mix(void)
{
    uint32_t t0 = cycles();
    int32_t vol = 0x1000, step = 7, vl = 0x5000, vr = 0x3000;
    for (int i = 0; i < 256; i++) {
        int32_t s = (k_out[i] * vol) >> 15;
        k_l[i] = sat16(k_l[i] + ((s * vl) >> 15));
        k_r[i] = sat16(k_r[i] + ((s * vr) >> 15));
        vol += step;
    }
    return cycles() - t0;
}

static uint32_t v_sink;
static uint32_t bench_sdram_read(uint32_t base)
{
    const volatile uint32_t *p = (const volatile uint32_t *)base;
    uint32_t sum = 0, t0 = cycles();
    for (int i = 0; i < 256; i++) sum += p[i];
    uint32_t dt = cycles() - t0;
    v_sink = sum;
    return dt;
}

/* ------------------------------------------------------------- tone */
/* 64-entry sine, amplitude 8000 (-12 dBFS), 440 Hz at 48 kHz. */
static const int16_t s_sine[64] = {
         0,    784,   1561,   2322,   3061,   3771,   4445,   5075,
      5657,   6184,   6652,   7055,   7391,   7656,   7846,   7961,
      8000,   7961,   7846,   7656,   7391,   7055,   6652,   6184,
      5657,   5075,   4445,   3771,   3061,   2322,   1561,    784,
         0,   -784,  -1561,  -2322,  -3061,  -3771,  -4445,  -5075,
     -5657,  -6184,  -6652,  -7055,  -7391,  -7656,  -7846,  -7961,
     -8000,  -7961,  -7846,  -7656,  -7391,  -7055,  -6652,  -6184,
     -5657,  -5075,  -4445,  -3771,  -3061,  -2322,  -1561,   -784
};

int main(void)
{
    w32(AUDIO_MBOX(AUDIO_MBOX_STATE), ST_TESTING);
    uint32_t scratch = r32(AUDIO_MBOX(1));

    result(0, test_mul());
    result(1, test_subword());
    result(2, scratch >= 0x40000000u && scratch < 0x44000000u && test_sdram(scratch));
    result(3, test_fifo());
    result(4, test_cycles());
#ifndef NO_CFU_TEST
    /* T5: the CFU answers, and one RS_OUT matches the C model's value:
     * acc 0 -> table row 0 {0x0c39, 0x66ad, 0x0d46, -0x21}, samples 1000 x4 ->
     * (1000*0x0c39+0x4000)>>15 + ... = 1000 (per the C model); adv 0 */
    ACFU(ACFU_RS_CFG, 0x8000, 0);
    uint32_t r5 = ACFU(ACFU_RS_OUT, (1000u << 16) | 1000u, (1000u << 16) | 1000u);
    result(5, ACFU(ACFU_ID, 0, 0) == ACFU_ID_VALUE && r5 == 1000u);
#endif

    for (int i = 0; i < 256 + 16; i++) k_in[i] = (int16_t)(i * 97 - 12000);
    for (int k = 0; k < 16; k++) k_coef[k] = (int16_t)(2048 - k * 100);
    w32(AUDIO_MBOX(5), bench_fir());
    w32(AUDIO_MBOX(6), bench_mix());
    w32(AUDIO_MBOX(7), (scratch >= 0x40000000u) ? bench_sdram_read(scratch) : 0);

    w32(AUDIO_MBOX(2), s_pass);
    w32(AUDIO_MBOX(3), s_fail);

    /* 440 Hz: phase step = 440 / 48000 * 2^32 */
    const uint32_t step = 39370534u;
    uint32_t phase = 0, pushed = 0;
    w32(CSR_APF_AUDIO_BUFFER_FLUSH_ADDR, 1);
    spin(64);
    w32(CSR_APF_AUDIO_PLAYBACK_EN_ADDR, 1);
    w32(AUDIO_MBOX(AUDIO_MBOX_STATE), ST_TONE);
    for (;;) {
        uint32_t fill = r32(CSR_APF_AUDIO_BUFFER_FILL_ADDR);
        while (fill < 3072u) {
            uint16_t s = (uint16_t)s_sine[phase >> 26];
            w32(CSR_APF_AUDIO_OUT_ADDR, ((uint32_t)s << 16) | s);   /* L = R */
            phase += step;
            fill++;
            pushed++;
        }
        w32(AUDIO_MBOX(4), pushed);
    }
}
