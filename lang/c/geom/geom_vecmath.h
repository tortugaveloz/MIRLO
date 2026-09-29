/* fp32-INTERFACE vec4/mat4 primitives for the geometry core -- backed
 * entirely by S15.16 pure-integer fixed point (geom_fixed.h), NOT a
 * hardware or soft FPU. This core matches the real N64's RSP, which had no
 * floating-point hardware at all (its vector ALUs are pure 16-bit fixed
 * point); the main
 * game CPU keeps the real hardware FPU it always had (VR4300 role), and
 * still builds/sends real float32 GDL words, so this file's job is to
 * consume/produce genuine IEEE-754 bit patterns while doing all actual
 * arithmetic in fixed point underneath -- see geom_fixed.h's header for the
 * full rationale and the S15.16-vs-CFU's-S17.10 format split.
 *
 * Every function below keeps its old `float` signature so geom_pipeline.c /
 * geom_triangle.c / geom_math.h need no changes at their call sites -- a
 * `float` local here just carries a 32-bit IEEE bit pattern between calls
 * (lw/sw only), decoded to fixed on the way into each op and re-encoded to
 * IEEE on the way out. (An earlier design gave the geom core a hardware
 * FPU; fixed point turned out smaller and faster.)
 *
 * FTOI keeps round-to-nearest (ties away from zero, via fx_to_int_round),
 * matching the convention the rest of the geometry/rasterizer pipeline (and
 * the old RTL FloatToInt) uses.
 *
 * The register-file-style setters/state (vpu_set_m/a/b/c/d, vpu_get_out,
 * vpu_load_matrix, ...) and the arithmetic ops below are real (non-inline)
 * functions defined once in geom_vecmath.c, not `static inline` here --
 * see geom_fixed.h's header comment for why (ROM budget: duplicating this
 * code into every one of geom_pipeline.c/geom_triangle.c/main.c overflowed
 * the geom core's 16KB ROM the first time this was built for the real
 * target). f2u/u2f/vf2fx/fx2vf stay inline: genuinely trivial one-liners,
 * cheap to duplicate, and inlining them lets the compiler fold the
 * union-cast away entirely at most call sites. */
#ifndef GEOM_VECMATH_H
#define GEOM_VECMATH_H

#include <stdint.h>
#include "geom_fixed.h"

static inline uint32_t f2u(float f)   { union { float f; uint32_t u; } x = { .f = f }; return x.u; }
static inline float    u2f(uint32_t u){ union { float f; uint32_t u; } x = { .u = u }; return x.f; }

/* float (IEEE bits) <-> S15.16 fixed, one hop -- every op below is built on
 * these two so the IEEE format only ever appears at a function's actual
 * boundary, never mid-expression. */
static inline int32_t vf2fx(float f)  { return fx_from_bits(f2u(f)); }
static inline float   fx2vf(int32_t x){ return u2f(fx_to_bits(x)); }

void  vpu_set_m(int i, float v);
void  vpu_set_a(int i, float v);
void  vpu_set_b(int i, float v);
void  vpu_set_c(int i, float v);
void  vpu_set_d(int i, float v);
float vpu_get_out(int i);

void vpu_load_matrix(const float m[16]);
void vpu_set_vecA(const float a[4]);
void vpu_set_vecB(const float b[4]);
void vpu_set_vecC(const float c[4]);

float   vpu_fmul(float a, float b);
float   vpu_fadd(float a, float b);
float   vpu_fsub(float a, float b);
float   vpu_frecip(float a);
float   vpu_itof(int32_t a);
/* Round to nearest (ties away from zero -- fx_to_int_round), matching the
 * old RTL FloatToInt convention this replaces. No fcvt.w.s, no lrintf. */
int32_t vpu_ftoi(float a);
/* num/den where BOTH are raw ints with no S15.16 range restriction (only the
 * quotient itself must fit) -- see fx_div_ii()'s comment. Use this instead
 * of vpu_frecip(vpu_itof(den)) whenever `num`/`den` can plausibly exceed
 * +-32767, e.g. triangle-setup's edge-function areas. */
float vpu_idiv(int32_t num, int32_t den);

void  vpu_matvec_run(void);
float vpu_dot4(void);
void  vpu_wsum3_run(void);
void  vpu_matvec(const float a[4], float out[4]);

#endif /* GEOM_VECMATH_H */
