/* fp32 vec4 / mat4 helpers for the geometry core's hardware FPU.
 *
 * Matrices are row-major, row-vector convention: v' = v * M, i.e.
 * out[j] = sum_i v[i] * M[i*4+j] -- what f3d_interp.c's mat4_transform()
 * uses too.
 */
#ifndef GEOM_MATH_H
#define GEOM_MATH_H

#include <stdint.h>
#include "geom_vecmath.h"
#include "geom_vpar.h"

/* Load the resident matrix M (row-major, 16 words) into the parallel
 * fixed-point CFU (rtl/vpu/Vpu4DFixed.v), converting float -> S17.10. Used by
 * load_vertices() once per vertex batch; the per-vertex MATVEC4 and the
 * lighting pass talk to the CFU directly (geom_vpar.h) in fixed point.
 * gm_mat4_mul() below (modelview*projection, once per matrix change) stays
 * on the scalar S15.16 path. */
static inline void gm_load_M(const float m[16]) {
    for (int i = 0; i < 16; i++) geom_vpar_load_m(i, geom_vpar_to_fixed(m[i]));
}

/* C = A * B  (row-major, row-vector: C[i][:] = A[i][:] * B); `c` may alias
 * `a` or `b`. See geom_vecmath.c. */
void gm_mat4_mul(float c[16], const float a[16], const float b[16]);
void gm_mat4_mul_fx(int32_t c[16], const int32_t a[16], const int32_t b[16]);

static inline void gm_identity_fx(int32_t m[16]) {
    for (int i = 0; i < 16; i++) m[i] = 0;
    m[0] = m[5] = m[10] = m[15] = FX_ONE;
}
static inline void gm_identity(float m[16]) {
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* scalar helpers (thin names over geom_vecmath.h) */
static inline float gm_mul(float a, float b)   { return vpu_fmul(a, b); }
static inline float gm_add(float a, float b)   { return vpu_fadd(a, b); }
static inline float gm_sub(float a, float b)   { return vpu_fsub(a, b); }
static inline float gm_recip(float a)          { return vpu_frecip(a); }
static inline float gm_i2f(int32_t a)          { return vpu_itof(a); }
static inline int32_t gm_f2i(float a)          { return vpu_ftoi(a); }

/* a*b + c */
static inline float gm_madd(float a, float b, float c) { return vpu_fadd(vpu_fmul(a, b), c); }

#endif /* GEOM_MATH_H */
