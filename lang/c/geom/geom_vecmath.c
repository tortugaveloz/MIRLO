/* Implementations for geom_vecmath.h -- see that file's header comment. */
#include "geom_vecmath.h"

static float _vs_m[16], _vs_a[4], _vs_b[4], _vs_c[4], _vs_d[4], _vs_out[4];

void  vpu_set_m(int i, float v) { _vs_m[i & 15] = v; }
void  vpu_set_a(int i, float v) { _vs_a[i & 3] = v; }
void  vpu_set_b(int i, float v) { _vs_b[i & 3] = v; }
void  vpu_set_c(int i, float v) { _vs_c[i & 3] = v; }
void  vpu_set_d(int i, float v) { _vs_d[i & 3] = v; }
float vpu_get_out(int i)        { return _vs_out[i & 3]; }

void vpu_load_matrix(const float m[16]) { for (int i = 0; i < 16; i++) _vs_m[i] = m[i]; }
void vpu_set_vecA(const float a[4])     { for (int i = 0; i < 4; i++) _vs_a[i] = a[i]; }
void vpu_set_vecB(const float b[4])     { for (int i = 0; i < 4; i++) _vs_b[i] = b[i]; }
void vpu_set_vecC(const float c[4])     { for (int i = 0; i < 4; i++) _vs_c[i] = c[i]; }

float vpu_fmul(float a, float b)   { return fx2vf(fx_mul(vf2fx(a), vf2fx(b))); }
float vpu_fadd(float a, float b)   { return fx2vf(fx_add(vf2fx(a), vf2fx(b))); }
float vpu_fsub(float a, float b)   { return fx2vf(fx_sub(vf2fx(a), vf2fx(b))); }
float vpu_frecip(float a)          { return fx2vf(fx_recip(vf2fx(a))); }
float vpu_itof(int32_t a)          { return fx2vf(fx_from_int(a)); }
int32_t vpu_ftoi(float a)          { return fx_to_int_round(vf2fx(a)); }
float vpu_idiv(int32_t num, int32_t den) { return fx2vf(fx_div_ii(num, den)); }

void vpu_matvec_run(void) {
    for (int j = 0; j < 4; j++) {
        int32_t s = 0;
        for (int i = 0; i < 4; i++) s = fx_add(s, fx_mul(vf2fx(_vs_a[i]), vf2fx(_vs_m[i * 4 + j])));
        _vs_out[j] = fx2vf(s);
    }
}
float vpu_dot4(void) {
    int32_t s = 0;
    for (int i = 0; i < 4; i++) s = fx_add(s, fx_mul(vf2fx(_vs_a[i]), vf2fx(_vs_b[i])));
    return fx2vf(s);
}
void vpu_wsum3_run(void) {
    for (int k = 0; k < 4; k++)
        _vs_out[k] = fx2vf(fx_wsum3(vf2fx(_vs_a[k]), vf2fx(_vs_d[0]),
                                     vf2fx(_vs_b[k]), vf2fx(_vs_d[1]),
                                     vf2fx(_vs_c[k]), vf2fx(_vs_d[2])));
}

void vpu_matvec(const float a[4], float out[4]) {
    vpu_set_vecA(a); vpu_matvec_run();
    for (int i = 0; i < 4; i++) out[i] = vpu_get_out(i);
}

/* C = A * B, row-major, row-vector. Same S15.16 arithmetic, in the same
 * order, as the old 4 x vpu_matvec_run() version (so results are
 * bit-identical), but each operand element is decoded from float once
 * instead of four times, and nothing is encoded until the end: 32 decodes
 * and 16 encodes per product instead of 128 and 16. It runs on every
 * modelview change, twice (the GDL_MTX multiply and the MVP update), so on
 * a real SM64 frame it was most of the geom core's remaining float
 * decoding. */
/* C = A * B in S15.16, row-major, row-vector; `c` may alias `a` or `b`.
 * The same per-product fx_mul() and saturating fx_add() the float version
 * below always did internally -- the GDL now carries S15.16 (it is what
 * SM64's own Mtx holds), so there is nothing to decode or re-encode. */
void gm_mat4_mul_fx(int32_t c[16], const int32_t a[16], const int32_t b[16]) {
    /* A product by 0 adds nothing and a product by 1.0 is the other factor --
     * both exactly, in S15.16 with fx_mul()'s rounding -- so they are
     * skipped: a perspective projection is 10/16 zeros and a modelview's
     * last column (0,0,0,1), which took most of the 64 multiplies (10 % of
     * the geom core's cycles on the JRB attract demo). */
    int32_t t[16];
    for (int row = 0; row < 4; row++)
        for (int j = 0; j < 4; j++) {
            int32_t s = 0;
            for (int i = 0; i < 4; i++) {
                int32_t x = a[row * 4 + i], y = b[i * 4 + j];
                if (x == 0 || y == 0) continue;
                s = fx_add(s, y == FX_ONE ? x : x == FX_ONE ? y : fx_mul(x, y));
            }
            t[row * 4 + j] = s;
        }
    for (int i = 0; i < 16; i++) c[i] = t[i];
}

void gm_mat4_mul(float c[16], const float a[16], const float b[16]) {
    int32_t A[16], B[16];
    for (int i = 0; i < 16; i++) { A[i] = vf2fx(a[i]); B[i] = vf2fx(b[i]); }
    for (int row = 0; row < 4; row++)
        for (int j = 0; j < 4; j++) {
            int32_t s = 0;
            for (int i = 0; i < 4; i++) s = fx_add(s, fx_mul(A[row * 4 + i], B[i * 4 + j]));
            c[row * 4 + j] = fx2vf(s);
        }
}
