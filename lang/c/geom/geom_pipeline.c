/* See geom_pipeline.h. Port of f3d_interp.c's interpreter to the GDL format,
 * with all fp32 math on the VPU. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "geom_vecmath.h"
#include "geom_math.h"
#include "geom_gdl.h"
#include "geom_dcache.h"
#include "geom_triangle.h"
#include "geom_pipeline.h"

#include "geom_rcp_tab.h"     /* NORMF seed table (normal_f), shared with the RTL */
#ifndef GEOM_HOST_TEST
#include "geom_cfg.h"
#endif
#if !defined(GEOM_HOST_TEST) || defined(GEOM_MRDP)
#include "geom_texfmt.h"
#endif
#include "geom_mrdp.h"

#define MTX_STACK_DEPTH 24
#define DL_STACK_DEPTH  16
#define VTX_CACHE_SIZE  32

/* ---- state -------------------------------------------------------------- */
static int32_t s_mv_stack[MTX_STACK_DEPTH][16];   /* S15.16, as the GDL carries it */
static int   s_mv_sp;
static int32_t s_proj[16];
static uint8_t s_pnz[4][4], s_pnz_n[4], s_pnz_ok;   /* mvp_mul(): s_proj's non-zero rows per column */
static uint8_t s_pkind;   /* mvp_mul(): 1 guPerspective's non-zeros, 2 guOrtho's, 0 any other */
static int32_t s_mvp[16];
static int32_t s_mvp_q[16];   /* s_mvp for the VPU, S17.10 scaled by 2^s_mvp_k[column] */
/* Per output column (x, y, z, w) of the MVP: its coefficients are loaded into
 * the VPU multiplied by 2^k and the result comes back << (6 - k) instead of
 * << 6. S17.10 has a fixed 1/1024 step, so a small coefficient -- the
 * skybox's guOrtho over 320 units is 0.00625, which quantised to 6/1024, 6 %
 * short -- lost most of its bits, and with coordinates in the thousands the
 * skybox tiles landed tens of pixels off, differently at every yaw (the "cut"
 * sky, always there on hardware). k is the largest <= 6 for which the column
 * cannot overflow the VPU's +-131072 range for any s16 object coordinate, so
 * an ordinary perspective MVP keeps k = 0, bit for bit what it had. */
static uint8_t s_mvp_sh[4] = { 6, 6, 6, 6 };   /* 6 - k, the output shift per column */
static bool  s_mvp_dirty;
static bool  s_lighting;
/* s_mvp_gen counts changes of s_mvp_q, so the VPU's copy (and PROJECT's
 * constants) are loaded again only when it changed or the lighting borrowed
 * the VPU's matrix -- not for every vertex batch. (Keeping each stack
 * level's MVP to restore on a POP was tried: once gm_mat4_mul_fx() skips its
 * zero and one products, the copies cost more than the multiply they saved.) */
static uint32_t s_mvp_gen = 1, s_vpu_m_gen, s_proj_cfg_gen;
static int32_t s_proj_cfg_vp[4];

/* Transformed vertex cache, S15.16. Kept in fixed point (not float) so the
 * clip tests, the clip path and triangle setup read it without decoding. */
/* ig: x and y both inside the screen guard band (screen_coord_in_guard()),
 * computed once per vertex -- emit_triangle() used to re-test all six
 * coordinates of every triangle (25% of its cycles). Kept current across a
 * GDL_VIEWPORT by refresh_guard_flags(). */
/* cx, cy, cz: the clip-space position the screen x/y/z were projected from,
 * kept for the clip (make_clip_vtx). Rebuilding it from the screen position
 * lost everything past S15.16's +-32768 px: a vertex near the camera or at a
 * steep angle projects far off-screen, saturates, and the clip then cut the
 * triangle along an edge that was not the real one. */
typedef struct { int32_t x, y, z, w, r, g, b, a, s, t; uint32_t fl; int32_t cx, cy, cz; } gvtx_t;
/* gvtx_t.fl: what emit_triangle() needs to accept or reject a triangle, per
 * vertex, computed once in load_vertices() (vtx_flags()) -- a triangle's
 * verdict is then an AND / OR of three words. Screen flags are refreshed on
 * GDL_VIEWPORT. VF_L..VF_B: beyond the framebuffer's left/right/top/bottom
 * edge; three vertices beyond the same one cover no pixel centre. */
#define VF_NEAR    0x001u   /* w <= GEOM_NEAR_W, or depth < 0 (projection's near plane) */
#define VF_FAR     0x002u   /* clip-space z > w */
#define VF_W0      0x004u   /* w == 0 exactly */
#define VF_ORTHO   0x008u   /* w within the orthographic band (0.98, 1.02) */
#define VF_NOGUARD 0x010u   /* screen x/y outside the side guard band */
#define VF_L       0x020u
#define VF_R       0x040u
#define VF_T       0x080u
#define VF_B       0x100u
#define VF_SCREEN  (VF_NOGUARD | VF_L | VF_R | VF_T | VF_B)
#define VF_OFFSCR  (VF_L | VF_R | VF_T | VF_B)
static gvtx_t s_vtx[VTX_CACHE_SIZE];

static int32_t s_vp_scale[4];   /* S15.16, already /4 by the game CPU */
static int32_t s_vp_trans[4];
static int32_t s_vp_fx[4];   /* see update_screen_guard() */
/* Cached screen-space guard-band bounds for emit_triangle()'s cheap "does
 * this triangle definitely need side-plane clipping" pre-check (see C5
 * comment near emit_triangle) -- "ordered" (monotonic unsigned, see f2ord)
 * keys of vp_trans[i] +/- (vp_scale[i] + GEOM_SIDE_GUARD_PX), recomputed
 * only when GDL_VIEWPORT changes (geom_reset()'s default counts as a change
 * too), so the common already-on-screen triangle pays zero extra VPU work. */
static int32_t s_screen_guard_lo[2], s_screen_guard_hi[2];   /* S15.16 */
/* the framebuffer's edges (GDL_FBSIZE), S15.16 */
static int32_t s_fb_w_fx = GEOM_FB_HRES * 65536, s_fb_h_fx = GEOM_FB_VRES * 65536;
static inline __attribute__((always_inline)) int32_t vtx_in_guard(int32_t x, int32_t y)
{
    return x >= s_screen_guard_lo[0] && x <= s_screen_guard_hi[0]
        && y >= s_screen_guard_lo[1] && y <= s_screen_guard_hi[1];
}
/* The screen part of gvtx_t.fl. Strictly beyond an edge: pixel centres sit
 * at +0.5 inside [0, HRES) x [0, VRES), so such a triangle covers none. */
static inline __attribute__((always_inline)) uint32_t vtx_screen_flags(int32_t x, int32_t y)
{
    uint32_t f = vtx_in_guard(x, y) ? 0u : VF_NOGUARD;
    if (x < 0) f |= VF_L;
    if (x > s_fb_w_fx) f |= VF_R;
    if (y < 0) f |= VF_T;
    if (y > s_fb_h_fx) f |= VF_B;
    return f;
}
static int32_t s_texnorm;     /* S15.16 1/tex_render_size, 0 = texturing off */
static uint32_t s_texenv;     /* GDL_TEXENV mode: GDL_TE_RGB_* | GDL_TE_A_* */
/* The render state last pushed to FEATURE_ENABLE / FRAGMENT_PIPELINE, as
 * emit_triangle()'s cfg key. Forgotten at the start of every display list:
 * the game CPU's per-frame clears (GDL_RAW, frame.c) rewrite
 * FRAGMENT_PIPELINE to turn depth_mask on for the depth clear, so what this
 * core last pushed is no longer what the rasterizer holds. */
static uint32_t s_cfg_emitted = 0xFFFFFFFFu;

static uint32_t s_rm = GDL_RM_DEFAULT;   /* GDL_RENDERMODE flags */
#ifdef GEOM_HOST_TEST
__attribute__((weak)) void geom_test_rendermode(uint32_t rm) { (void)rm; }
#endif
#ifdef GEOM_HOST_TEST
__attribute__((weak)) void geom_test_texenv(uint32_t mode) { (void)mode; }
#endif
static int32_t s_texnorm_s_fx = FX_ONE, s_texnorm_t_fx = FX_ONE;  /* the same, S15.16 (emit_triangle) */

static bool  s_fog_on;
/* GDL_CCOLOR: the combiner's PRIMITIVE/ENVIRONMENT constants, folded into
 * the vertex colours of every triangle drawn while it is on. */
static uint32_t s_cc_flags;                  /* bit0 on, bit1 rgb * shade, bit2 alpha * shade */
static uint32_t s_cc_mul;                    /* per channel r,g,b,a: 1 = shade * K, 0 = K */
static int32_t  s_cc_k[4];                   /* K per channel, fx; set by GDL_CCOLOR */
static int32_t s_fog_rgb[3];                 /* S15.16 */
static int32_t s_fog_mul, s_fog_off;         /* S15.16 */

/* directional Gouraud lighting (GDL_LIGHT) -- replaces the flat-grey stub */
static int   s_num_lights;
/* How many matrices the degenerate-modelview guard dropped. Outside the
 * target-only block on purpose: the host reference harness compiles this same
 * file and must see the same guard, and s_diag does not exist there. */
unsigned long geom_mtx_dropped;

/* Reject a modelview that cannot be a transform.
 *
 * Measured on the Pocket: 52 of the 59 modelview matrices in one game
 * frame arrived with m[3][3] == 0, against 0 of 62 on the host. A zero there
 * makes every transformed w come out zero, the perspective divide blows up
 * and the triangle smears across the whole scanline (horizontal banding),
 * which matches the census attributing 98% of rejections to
 * all3_w_out. The 52 are byte-identical to each other and their values appear
 * nowhere in the host's display list, so they are foreign memory reached
 * through a wrong pointer somewhere in the game CPU's emit path.
 *
 * That root cause is still open and the fix belongs upstream. This is a
 * containment guard, not the fix: a matrix whose bottom-right is not 1.0 is
 * never a valid affine transform, so consuming it can only produce garbage.
 * Dropping it leaves the previous modelview in place, which costs at worst a
 * misplaced object instead of destroying the frame.
 *
 * Deliberately NOT applied to the projection: GDLPROJ is bit-identical to the
 * host, the projection's m[3][3] is legitimately 0 for a perspective matrix,
 * and guarding it would reject every correct frame.
 *
 * s_diag[12] counts what this drops, so the guard cannot hide the defect. */
static inline bool geom_mtx_usable(const uint32_t *m, uint32_t target)
{
    if (target == GDL_MTX_TARGET_PROJECTION) return true;
    return m[15] == (uint32_t)FX_ONE;   /* exactly 1.0 in S15.16 */
}

/* Instrumentation -- the rejection census (s_diag, published to
 * GEOM_DIAG_ADDR each frame) and the per-frame cycle profile (PR_*) -- is
 * built only with -DGEOM_INSTRUMENT: the 16 KB ROM is full, and the ROM build
 * leaves it out. DIAG_INC / PROF_CNT / PROF_T0 / PROF_ADD vanish without it. */
#if !defined(GEOM_HOST_TEST) && defined(GEOM_INSTRUMENT)
#define GEOM_TARGET_INSTRUMENT 1
/* Rejection census accumulated in this core's own RAM, then published to
 * SDRAM once per frame with an explicit D-cache flush.
 *
 * Counting straight into SDRAM does NOT work: the geom core's writes sit in
 * its write-back D-cache, which is not coherent with anything, so the game
 * CPU reads whatever SDRAM happened to hold. That is what produced
 * "in=1075631678 inside0=291213813" -- numbers that look like a wild pointer
 * and are really just uninitialised memory. The same non-coherence is already
 * documented for the framebuffer and for the GDL; it applies here too. */
/* [0..7] are counters, cumulative since boot (geom_reset runs once, before
 * the doorbell loop -- main.c:49). [8..11] are the per-frame GDL checksum. */
static uint32_t s_diag[GEOM_DIAG_N];

/* Per-frame cycle profile, published in diag slots 13..24 (rdcycle, sys clock).
 * Nested: PR_TRI contains PR_CLIP, PR_SETUP, PR_CFG and PR_STREAM; PR_STREAM
 * contains PR_STALL (time spent waiting on a full rasterizer command FIFO,
 * i.e. time the geom core is blocked on someone else, not computing). */
enum { PR_TOTAL, PR_VERT, PR_TRI, PR_SETUP, PR_STREAM, PR_CFG, PR_CLIP, PR_TEX,
       PR_STALL, PR_WORDS, PR_NVERT, PR_NTRI, PR_COUNT };
static uint32_t s_prof[PR_COUNT];
#define PROF_T0(v)      uint32_t v = prof_now()
#define PROF_ADD(i, v)  (s_prof[i] += prof_now() - (v))
#define PROF_CNT(i, n)  (s_prof[i] += (n))
#define DIAG_INC(i)     (s_diag[i]++)
#else
#define PROF_T0(v)      ((void)0)
#define PROF_ADD(i, v)  ((void)0)
#define PROF_CNT(i, n)  ((void)0)
#define DIAG_INC(i)     ((void)0)
#endif
#ifdef GEOM_HOST_TEST
/* Host mirror of the geom core's GEOM_DIAG census, so the two sides report
 * the same five numbers and are directly comparable. Defined here rather than
 * in one harness: every harness links this file, only one of them reads it. */
long h_diag[8];
#endif
static int   s_any_light;   /* any GDL_LIGHT seen since geom_reset() */
static int32_t s_light_dir[8][3];   /* S15.16 eye space, /127, NOT renormalised -- see update_obj_lights() */
static int32_t s_lookat_dir[2][3];  /* GDL_LIGHT_LOOKAT_X/Y, the same format */
static int32_t s_lookat_m[2][3];    /* ... in object space, normalised, S17.10 (update_obj_lights) */
static bool    s_texgen;            /* GDL_GEOMODE_TEXGEN */
static int32_t s_light_col_q[8][3]; /* S15.16, colour/255 */
static int32_t s_ambient_q[3];      /* S15.16 */
/* Light directions moved into OBJECT space (MV3 * L, renormalised) as
 * S17.10, laid out as the CFU's M: light k of group g is column k&3 of
 * s_lit_m[k>>2]. Row 3 stays 0 (A[3] is always 0 anyway). Recomputed only
 * when the modelview or a light changes (s_lit_dirty), not per batch. */
static int32_t s_lit_m[2][16];
static bool    s_lit_dirty;
/* update_obj_lights()'s inputs at its last run: the modelview's 3x3, then
 * up to 4 light directions -- a modelview change that leaves them alone (a
 * translation, a PUSH/POP back to the same rotation; SM64 re-sends the same
 * lights with every object) leaves s_lit_m as it is */
static int32_t  s_lit_key[9 + 12];
static int      s_lit_key_n = -1;


static geom_raster_state_t s_rs;

static const uint32_t *s_dl_stack[DL_STACK_DEPTH];
static int s_dl_sp;

/* ---- setup ------------------------------------------------------------- */
static void update_screen_guard(void);


void geom_reset(void)
{
#ifdef GEOM_SETUP_HW
    geom_setup_probe();          /* before anything below converts or divides */
#endif
#ifdef GEOM_TARGET_INSTRUMENT
    for (int i_ = 0; i_ < GEOM_DIAG_N; i_++) s_diag[i_] = 0;
#endif
#if defined(GEOM_HOST_TEST) && defined(F4_TRICOUNT_PROBE)
    { for (int i_=0;i_<8;i_++) h_diag[i_]=0; }
    /* One display-list walk == one frame. Latch the previous frame's count so
     * the harness can report it exactly the way the hardware GDLDIAG line
     * does, which is the only way the two numbers are comparable. */
    { extern long g_tris_this_frame, g_tris_last_frame;
      g_tris_last_frame = g_tris_this_frame; g_tris_this_frame = 0; }
#endif
    gm_identity_fx(s_mv_stack[0]);
    s_mv_sp = 0;
    gm_identity_fx(s_proj);
    s_mvp_dirty = true;
    s_vpu_m_gen = 0; s_proj_cfg_gen = 0; s_pnz_ok = 0;   /* reload the VPU's matrix and PROJECT's constants */
    s_lighting = false;
    geom_set_cull(1);
    s_texnorm = 0;
    s_texnorm_s_fx = s_texnorm_t_fx = FX_ONE;
    s_fog_on = false;
    s_cc_flags = 0;
    s_dl_sp = 0;

    s_num_lights = 0;
    s_any_light = 0;
    s_lit_dirty = true;
    s_texgen = false;
    memset(s_lookat_dir, 0, sizeof(s_lookat_dir));
    s_lit_key_n = -1;
    s_ambient_q[0] = s_ambient_q[1] = s_ambient_q[2] = 0;
    for (int i = 0; i < 8; i++) {
        s_light_dir[i][0] = s_light_dir[i][1] = 0; s_light_dir[i][2] = FX_ONE;
        s_light_col_q[i][0] = s_light_col_q[i][1] = s_light_col_q[i][2] = 0;
    }

    /* default viewport: full framebuffer, matches f3d_interp.c's 160*4 etc.
     * (the game CPU normally overrides via GDL_VIEWPORT). */
    s_vp_scale[0] = GEOM_FB_HRES << 15; s_vp_scale[1] = GEOM_FB_VRES << 15;   /* x 0.5 in S15.16 */
    s_vp_scale[2] = s_vp_scale[3] = 0;
    s_vp_trans[0] = GEOM_FB_HRES << 15; s_vp_trans[1] = GEOM_FB_VRES << 15;
    s_vp_trans[2] = s_vp_trans[3] = 0;
    update_screen_guard();

    memset(&s_rs, 0, sizeof(s_rs));
    s_rs.scissor_enable = true;
    s_rs.tmu_enable[0] = false;
    geom_raster_set_scissor(&s_rs, 0, 0, GEOM_FB_HRES, GEOM_FB_VRES);
}

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

/* floor(sqrt(x)) for a 64-bit x, the same digit-by-digit loop as
 * isqrt_u32() -- only constant shifts, so no libgcc helper on rv32. Used to
 * normalise a light direction (update_obj_lights(), once per light per
 * modelview change, not per vertex). */
static uint32_t isqrt_u64(uint64_t x)
{
    if (!(x >> 32)) return isqrt_u32((uint32_t)x);   /* floor(sqrt) is unique: same answer in 32 bits */
    /* b = the largest power of 4 <= x, from x's top bit (the loop that
     * walked it down from 2^62 took ~15 64-bit compares) */
    uint32_t hi = (uint32_t)(x >> 32), t = hi ? hi : (uint32_t)x;
    int m = 0;
    if (t >> 16) { t >>= 16; m += 16; }
    if (t >> 8)  { t >>= 8;  m += 8; }
    if (t >> 4)  { t >>= 4;  m += 4; }
    if (t >> 2)  { t >>= 2;  m += 2; }
    uint64_t r = 0, b = hi ? (uint64_t)(1u << m) << 32 : (uint64_t)(x ? 1u << m : 0u);
    while (b) {
        if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
        else            r >>= 1;
        b >>= 2;
    }
    return (uint32_t)r;
}

/* S15.16 -> the VPU's S17.10: divide by 64, halves away from zero (what the
 * float decode this replaces did). |v| < 2^31 leaves 2^25, inside 27 bits. */
static inline int32_t fx16_to_vpar(int32_t v)
{
    return v >= 0 ? (v + 32) >> 6 : -((32 - v) >> 6);
}

/* mvp = mv * proj, as gm_mat4_mul_fx() computes it (the same products in
 * the same order, zeros skipped), but walking only each projection column's
 * non-zero entries, listed once per projection: a perspective one has five,
 * so 20 products per MVP instead of 64 tests. */
/* one product of mvp_mul(): x * y as gm_mat4_mul_fx() takes it (a zero
 * factor makes a zero product, which adds nothing to the column's
 * saturating sum: skipped) */
static inline int32_t mvp_p(int32_t x, int32_t y)
{
    if (x == 0 || y == 0) return 0;
    return y == FX_ONE ? x : x == FX_ONE ? y : fx_mul(x, y);
}

static void mvp_mul(int32_t c[16], const int32_t a[16])
{
    if (!s_pnz_ok) {
        uint32_t nz = 0;
        for (int j = 0; j < 4; j++) {
            s_pnz_n[j] = 0;
            for (int i = 0; i < 4; i++) if (s_proj[i * 4 + j]) { s_pnz[j][s_pnz_n[j]++] = (uint8_t)i; nz |= 1u << (i * 4 + j); }
        }
        /* the two shapes SM64 uses, straight-line below: guPerspective
         * (0,0) (1,1) (2,2) (2,3) (3,2) and guOrtho (0,0) (1,1) (2,2) (3,x) */
        s_pkind = !(nz & ~0x4C21u) ? 1 : !(nz & ~0xF421u) ? 2 : 0;   /* or a subset of one */
        s_pnz_ok = 1;
    }
    if (s_pkind) {
        /* the same products summed in the same order (rows ascending) */
        const int32_t p00 = s_proj[0], p11 = s_proj[5], p22 = s_proj[10];
        const int32_t p23 = s_proj[11], p30 = s_proj[12], p31 = s_proj[13], p32 = s_proj[14], p33 = s_proj[15];
        for (int r = 0; r < 4; r++, a += 4, c += 4) {
            const int32_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
            if (s_pkind == 1) {
                c[0] = mvp_p(a0, p00);
                c[1] = mvp_p(a1, p11);
                c[2] = fx_add(mvp_p(a2, p22), mvp_p(a3, p32));
                c[3] = mvp_p(a2, p23);
            } else {
                int32_t t3 = mvp_p(a3, p30);
                c[0] = fx_add(mvp_p(a0, p00), t3);
                c[1] = fx_add(mvp_p(a1, p11), mvp_p(a3, p31));
                c[2] = fx_add(mvp_p(a2, p22), mvp_p(a3, p32));
                c[3] = mvp_p(a3, p33);
            }
        }
        return;
    }
    for (int r = 0; r < 4; r++)
        for (int j = 0; j < 4; j++) {
            int32_t s = 0;
            for (int k = 0; k < s_pnz_n[j]; k++) {
                int i = s_pnz[j][k];
                int32_t x = a[r * 4 + i], y = s_proj[i * 4 + j];
                if (x == 0) continue;
                s = fx_add(s, y == FX_ONE ? x : x == FX_ONE ? y : fx_mul(x, y));
            }
            c[r * 4 + j] = s;
        }
}

static void update_mvp(void)
{
    if (s_mvp_dirty) {
        /* row-vector: modelview on the LEFT (mvp = mv * proj) */
        mvp_mul(s_mvp, s_mv_stack[s_mv_sp]);
        for (int j = 0; j < 4; j++) {
            /* bound on |out_j| in S15.16 raw units: |x|,|y|,|z| <= 32768, w = 1 */
            uint32_t am[4];
            for (int r = 0; r < 4; r++) {
                int32_t m = s_mvp[r * 4 + j];
                am[r] = m < 0 ? 0u - (uint32_t)m : (uint32_t)m;
            }
            /* b = (am0 + am1 + am2) << 15 + am3 in two words, hi:lo; the
             * largest k <= 6 with (b << (k+1)) < 2^33, i.e. b < 2^(32-k):
             * none if hi != 0, else 1 + lo's leading zeros (at most 5 counted) */
            uint32_t s0 = am[0] + am[1], cy = s0 < am[0];
            uint32_t s1 = s0 + am[2]; cy += s1 < s0;
            uint32_t lo = s1 << 15, hi = cy << 15 | s1 >> 17;
            lo += am[3]; hi += lo < am[3];
            int k = 0;
            if (hi == 0) {
                k = 1;
                while (k < 6 && !(lo & (0x80000000u >> (k - 1)))) k++;
            }
            /* The x/y/z rows go to the CFU exact: MATVEC_OB_HP (0x34) takes
             * the vertex at S17.4, 6 bits below the constant 1's S17.10, so
             * those rows are held << (6 - sh) instead of >> sh -- no fraction
             * bit dropped. They used to lose sh (5-6 for level geometry, whose
             * world coordinates and camera translation run to thousands of
             * units): up to ~6 clip units of error on a vertex, different
             * every frame, so floors and walls jittered and a shadow lost the
             * depth test against its floor (HMC). Only the translation row
             * (times the constant 1) is still shifted: a constant 2^(sh-17)
             * units. A row entry that would not fit 27 bits raises sh. */
            uint32_t am3 = am[0] > am[1] ? am[0] : am[1];
            if (am[2] > am3) am3 = am[2];
            while (k > 0 && am3 >= (1u << (26 - k))) k--;   /* (am3 << k) >= 2^26 */
            int sh = 6 - k;
            s_mvp_sh[j] = (uint8_t)sh;
            for (int r = 0; r < 4; r++) {
                int32_t v = s_mvp[r * 4 + j];
                int32_t h = sh ? (1 << (sh - 1)) : 0;   /* round half away from zero, as fx16_to_vpar */
                s_mvp_q[r * 4 + j] = r < 3 ? (int32_t)((uint32_t)v << k)
                                           : v >= 0 ? (v + h) >> sh : -((h - v) >> sh);
            }
        }
        s_mvp_dirty = false;
        s_mvp_gen++;
#ifdef GEOM_DIAG
        extern int g_diag_mvp_dump;
        if (g_diag_mvp_dump > 0) {
            g_diag_mvp_dump--;
            extern int printf(const char *, ...);
            const int32_t *mv = s_mv_stack[s_mv_sp];
#define F16(v) ((double)(v) / 65536.0)
            printf("  MV (sp=%d):\n", s_mv_sp);
            for (int r = 0; r < 4; r++)
                printf("    % .5f % .5f % .5f % .5f\n", F16(mv[r*4+0]), F16(mv[r*4+1]), F16(mv[r*4+2]), F16(mv[r*4+3]));
            printf("  PROJ:\n");
            for (int r = 0; r < 4; r++)
                printf("    % .5f % .5f % .5f % .5f\n", F16(s_proj[r*4+0]), F16(s_proj[r*4+1]), F16(s_proj[r*4+2]), F16(s_proj[r*4+3]));
            printf("  MVP:\n");
            for (int r = 0; r < 4; r++)
                printf("    % .5f % .5f % .5f % .5f\n",
                       F16(s_mvp[r*4+0]), F16(s_mvp[r*4+1]), F16(s_mvp[r*4+2]), F16(s_mvp[r*4+3]));
#undef F16
        }
#endif
    }
}

/* ---- vertex load ---------------------------------------------------- */
/* NDC z in [-1,1] -> depth-buffer [0,1), with the 65534/65536 safety margin
 * that keeps the far end clear of the depth buffer's exact-1.0 clamp (see
 * load_vertices()). GEOM_DEPTH_INV_K2 inverts it: ndc_z = depth*INV_K2 - 1,
 * which the clip loop needs to recover clip-space z from the stored depth. */
#define GEOM_DEPTH_K       (65534.0f / 65536.0f)
#define GEOM_DEPTH_INV_K2  (2.0f * (65536.0f / 65534.0f))

/* A vertex is inside the projection's REAL near plane iff its NDC z >= -1,
 * i.e. iff the depth load_vertices() stored is >= 0 -- a sign-bit test on a
 * value already computed, so this costs nothing on the hot path.
 *
 * This is a genuinely different plane from GEOM_NEAR_W above. That one is a
 * division guard, fixed at w >= 4 (z_eye = -4) regardless of the projection;
 * the projection actually in use across these tests (m[14] = -40) has its
 * near plane at z_eye = -20. Everything between the two survived the w clip
 * with NDC z far outside [-1,1] -- depths of 1.97 and -2.6 were measured on
 * the T7.6 scene -- and the rasterizer's depth clamp maps any such value onto
 * the same extreme, making those fragments fail the depth test against the clear
 * value and simply vanish. That is exactly the T7.6 hardware divergence:
 * 14491 covered pixels where the host, which keeps depth in float and so
 * cannot show the symptom, covers 52506. */
static inline int geom_w_outside(int32_t w)
{
    return w <= 4 * FX_ONE;   /* negative, or inside the GEOM_NEAR_W (4) guard */
}
/* The orthographic band, 0.98 < w < 1.02 in S15.16 (64225.3 .. 66846.7):
 * SM64's HUD, skybox and fades have w ~ 1 by construction. */
#define GEOM_ORTHO_W_LO_FX 64225
#define GEOM_ORTHO_W_HI_FX 66847
static inline int geom_z_outside(int32_t depth)
{
    return depth < 0;
}

/* ---- lighting on the CFU ---------------------------------------------
 * Same scheme as the real N64 RSP (F3DEX2): instead of transforming every
 * vertex normal into eye space and renormalising it there (the old path:
 * ~40 scalar fixed-point ops per lit vertex, each paying a float-bits
 * decode/encode), the light DIRECTIONS are moved into object space once per
 * modelview/light change, L'_k = normalize(MV3 * L_k), and each vertex
 * dots its raw object-space normal against all of them. That dot is one
 * MATVEC4: the (up to) 4 object-space lights are M's columns, the unit
 * normal is A (A[3] = 0), and out[k] = N . L'_k for all 4 lights at once.
 * The colour sum and clamp that follow are native integer ops.
 *
 * Why this is the same answer: with the row-vector convention the eye-space
 * normal is n*MV3, so (n*MV3) . L = n . (MV3*L). For a rotation times a
 * uniform scale s, |n*MV3| = s|n| and |MV3*L| = s|L|, so normalising the
 * light instead of the transformed normal gives the identical dot product
 * (measured on 38400 random lit vertices: <= 0.46 LSB of 8-bit colour from
 * the exact answer, the old path <= 0.11). Under NON-uniform scale neither
 * is exact -- that needs the inverse-transpose, which the old path never
 * did either -- and on a strong 1:0.5:1.7 scale both sit ~30 LSB (mean)
 * from it; this one is at least what the N64's own microcode computes.
 *
 * Normals are still renormalised per vertex (T11.5: SM64's normals are not
 * reliably unit length), but in object space on the raw int8 values, so it
 * is an integer square root and one integer divide, not a float rsqrt.
 * S17.10 is precise enough here, unlike the screen map (see load_vertices):
 * every operand is a unit vector or a colour, the output is 8-bit colour. */

static void update_obj_lights(void)
{
    const int32_t *mv = s_mv_stack[s_mv_sp];
    s_lit_dirty = false;
    int same = s_lit_key_n == s_num_lights;
    s_lit_key_n = s_num_lights <= 4 ? s_num_lights : -1;
    int32_t *key = s_lit_key;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++, key++) {
            if (*key != mv[r * 4 + c]) same = 0;
            *key = mv[r * 4 + c];
        }
    for (int k = 0; k < s_num_lights && k < 4; k++)
        for (int c = 0; c < 3; c++, key++) {
            if (*key != s_light_dir[k][c]) same = 0;
            *key = s_light_dir[k][c];
        }
    if (same && !s_texgen) return;
    for (int k = 0; k < s_num_lights + (s_texgen ? 2 : 0); k++) {
        const int32_t *L = k < s_num_lights ? s_light_dir[k] : s_lookat_dir[k - s_num_lights];
        int32_t l[3];
        for (int i = 0; i < 3; i++)
            l[i] = fx_add(fx_add(fx_mul(mv[i*4+0], L[0]), fx_mul(mv[i*4+1], L[1])),
                          fx_mul(mv[i*4+2], L[2]));
        /* |l| in S15.16 = sqrt(sum l_i^2), the sum at scale 2^32 */
        uint64_t l2 = 0;
        for (int i = 0; i < 3; i++) l2 += (uint64_t)((int64_t)l[i] * l[i]);
        uint32_t len = l2 ? isqrt_u64(l2) : 0;
        int32_t *m = &s_lit_m[k >> 2][k & 3];
        int32_t la_tmp[12];
        if (k >= s_num_lights) m = la_tmp;          /* a look-at: stride 4 as below, copied out after */
        if (len >= FX_RECIP_NORM_MIN && len <= 0x80000000u) {
            /* l_i / |l| to full precision (fx_div_ii's fx_recip keeps ~10 bits
             * once |l| is in the hundreds) */
            fx_recip_norm_t kr = fx_recip_norm(len);
            for (int i = 0; i < 3; i++)
#ifdef GEOM_FX_DIVN_LAST
                m[i * 4] = fx16_to_vpar(fx_div_norm_last(l[i], kr.r));
#else
                m[i * 4] = fx16_to_vpar(fx_div_norm(l[i], kr));
#endif
        } else {
            for (int i = 0; i < 3; i++)
                m[i * 4] = len ? fx16_to_vpar(fx_div_ii(l[i], (int32_t)len)) : 0;
        }
        if (k >= s_num_lights)
            for (int i = 0; i < 3; i++) s_lookat_m[k - s_num_lights][i] = la_tmp[i * 4];
    }
    s_lit_dirty = false;
}

/* Writes the lit colour into ov[i].r/g/b (S15.16); alpha (ov[i].a) is not
 * lit. */
/* Renormalisation factor of an int8 normal of squared length len2 -- see the
 * comment in light_vertices(). */
static inline int32_t normal_f(uint32_t len2)
{
#ifdef GEOM_SETUP_HW
    if (g_su_normf) return geom_setup_normf(len2);        /* GeomSetupUnit 0x54: same value */
#endif
    /* ~2^20 / sqrt(len2) from a 1/sqrt seed table, the N64 RSP's VRSQ way
     * (tools/gen_rcp_tab.py): 2^-11 relative for real normals, well inside
     * the S17.10 the result feeds. GeomSetupUnit.v 0x54 is the same steps. */
    if (!len2) return 0;
    int msb = 0; uint32_t t = len2;                 /* len2 < 2^16 (int8 normal) */
    if (t >> 8) { t >>= 8; msb += 8; }
    if (t >> 4) { t >>= 4; msb += 4; }
    if (t >> 2) { t >>= 2; msb += 2; }
    if (t >> 1) {           msb += 1; }
    int32_t r = geom_rsq_tab[((uint32_t)(msb & 1) << 8) | (((len2 << (15 - msb)) >> 7) & 255u)];
    int sh = 4 - ((msb & ~1) >> 1);
    return sh >= 0 ? r << sh : r >> -sh;
}

/* -O3 alone here: its per-vertex loop unrolled and scheduled, -1.5 % of a frame
 * (sim/geom_full); the whole file at -O3 does not fit the ROM */
__attribute__((optimize("O3"))) static void light_vertices(const geom_vtx_t *src, int n, gvtx_t *ov)
{
    /* Flat-grey fallback for geometry that arrives with lighting enabled but
     * no light ever set (see the general path's final loop). */
    if (!s_any_light) {
        for (int i = 0; i < n; i++) ov[i].r = ov[i].g = ov[i].b = 39322;
        return;
    }
    /* Common case, <= 4 directional lights = one MATVEC4 group: one pass per
     * vertex with the sum in registers. Same terms in the same order as the
     * general path below (ambient, then light 0..nl-1, then the clamp), which
     * is what SM64 always sends -- the general path's per-vertex array and
     * its separate init/clamp passes cost ~25% of lighting. */
    if (s_num_lights <= 4) {
        const int32_t am0 = s_ambient_q[0], am1 = s_ambient_q[1], am2 = s_ambient_q[2];
        const int nl = s_num_lights;
        /* N . L'_k on the CPU: MATVEC4's (sum + 512) >> 10 exactly -- normals
         * and light directions are unit vectors in S17.10, so the sum never
         * nears the CFU's 27-bit saturation -- and it leaves the MVP resident
         * in the CFU for the next load_vertices() instead of swapping the
         * light matrix in and out around every vertex batch. */
        int32_t L[4][3];
        for (int k = 0; k < nl; k++)
            for (int j = 0; j < 3; j++) L[k][j] = s_lit_m[0][j * 4 + k];
        const int32_t L0x = nl ? L[0][0] : 0, L0y = nl ? L[0][1] : 0, L0z = nl ? L[0][2] : 0;
        const int32_t C0r = s_light_col_q[0][0], C0g = s_light_col_q[0][1], C0b = s_light_col_q[0][2];
        uint32_t last_len2 = 0; int32_t last_f = 0;
        for (int i = 0; i < n; i++) {
            int32_t r = am0, g = am1, b = am2;
            if (nl > 0) {
                const int8_t *cn = (const int8_t *)src[i].cn;
                uint32_t len2 = (uint32_t)(cn[0]*cn[0] + cn[1]*cn[1] + cn[2]*cn[2]);
                if (len2 != last_len2) { last_len2 = len2; last_f = normal_f(len2); }
                int32_t f = last_f;
                const int32_t n0 = (cn[0] * f + 512) >> 10, n1 = (cn[1] * f + 512) >> 10, n2 = (cn[2] * f + 512) >> 10;
                if (s_texgen) {
                    /* F3DEX2 texgen: (N.lookat + 1) / 4 of gsSPTexture's scale,
                     * in texel/32 units; S17.10 dots, so +1 is +1024 */
                    const int32_t *lx = s_lookat_m[0], *ly = s_lookat_m[1];
                    int32_t ds = (n0 * lx[0] + n1 * lx[1] + n2 * lx[2]) >> 10;
                    int32_t dt = (n0 * ly[0] + n1 * ly[1] + n2 * ly[2]) >> 10;
                    ov[i].s = (ds + 1024) << 15;
                    ov[i].t = (dt + 1024) << 15;
                }
                if (nl == 1) {                                 /* SM64's usual Lights1 */
                    int32_t d = (n0 * L0x + n1 * L0y + n2 * L0z + 512) >> 10;
                    if (d > 0) { r += (C0r * d) >> 10; g += (C0g * d) >> 10; b += (C0b * d) >> 10; }
                } else
                for (int k = 0; k < nl; k++) {
                    int32_t d = (n0 * L[k][0] + n1 * L[k][1] + n2 * L[k][2] + 512) >> 10;
                    if (d <= 0) continue;                      /* max(0, d) */
                    const int32_t *lc = s_light_col_q[k];
                    r += (lc[0] * d) >> 10; g += (lc[1] * d) >> 10; b += (lc[2] * d) >> 10;
                }
            }
            ov[i].r = r > FX_ONE ? FX_ONE : r;
            ov[i].g = g > FX_ONE ? FX_ONE : g;
            ov[i].b = b > FX_ONE ? FX_ONE : b;
        }
        return;
    }
    int32_t acc[VTX_CACHE_SIZE][3];                       /* S15.16 */
    for (int i = 0; i < n; i++)
        for (int c = 0; c < 3; c++) acc[i][c] = s_ambient_q[c];
    for (int g = 0; g * 4 < s_num_lights; g++) {
        for (int e = 0; e < 16; e++) geom_vpar_load_m(e, s_lit_m[g][e]);
        s_vpu_m_gen = 0;
        int nl = s_num_lights - g * 4; if (nl > 4) nl = 4;
        uint32_t last_len2 = 0; int32_t last_f = 0;
        for (int i = 0; i < n; i++) {
            const int8_t *cn = (const int8_t *)src[i].cn;
            uint32_t len2 = (uint32_t)(cn[0]*cn[0] + cn[1]*cn[1] + cn[2]*cn[2]);
            /* isqrt(len2<<8) = 16|n| (4 extra bits of precision); f = 2^20/|n|,
             * so (n_i*f)>>10 = n_i/|n| in S17.10. A zero normal gets f = 0,
             * i.e. ambient only, same as before. f depends on len2 alone and
             * neighbouring vertices usually share a normal (or at least its
             * length), so the square root + divide is only redone when len2
             * changes -- it was ~14% of the vertex loop on a real frame. */
            if (len2 != last_len2) {
                last_len2 = len2;
                last_f = normal_f(len2);
            }
            int32_t f = last_f;
            for (int j = 0; j < 3; j++) geom_vpar_load_a(j, (cn[j] * f + 512) >> 10);
            geom_vpar_load_a(3, 0);
            geom_vpar_matvec4();
            for (int k = 0; k < nl; k++) {
                int32_t d = geom_vpar_read_out(k);            /* N . L'_k, S17.10 */
                if (d <= 0) continue;                          /* max(0, d) */
                const int32_t *lc = s_light_col_q[g * 4 + k];
                for (int c = 0; c < 3; c++) acc[i][c] += (lc[c] * d) >> 10;
            }
        }
    }
    for (int i = 0; i < n; i++)
        for (int c = 0; c < 3; c++)
            /* Flat-grey fallback for geometry that arrives with lighting
             * enabled but no light ever set. Keyed off "no light command at
             * all", not s_num_lights == 0, so an ambient-only setup shades to
             * the ambient colour as the lighting equation says. 39322 = 0.6 in S15.16. */
            (&ov[i].r)[c] = !s_any_light ? 39322 : acc[i][c] > FX_ONE ? FX_ONE : acc[i][c];
}

/* S17.10 CFU output -> S15.16, saturating (the CFU's range is 4x wider). */
/* VPU output of MVP column j -> S15.16: << s_mvp_sh[j], saturating. */
static inline __attribute__((always_inline)) int32_t vpar_col_to_fx(int32_t raw, int sh)
{
    if (raw > (FX_MAX_RAW >> sh)) return FX_MAX_RAW;
    if (raw < (FX_MIN_RAW >> sh)) return FX_MIN_RAW;
    return (int32_t)((uint32_t)raw << sh);
}

static inline __attribute__((always_inline)) int32_t vpar_to_fx(int32_t raw)
{
    if (raw > (FX_MAX_RAW >> 6)) return FX_MAX_RAW;
    if (raw < (FX_MIN_RAW >> 6)) return FX_MIN_RAW;
    return raw * 64;
}

static inline float fx_f(int32_t x) { return u2f(fx_to_bits(x)); }

/* Perspective divide x / w in S15.16. fx_recip(w) is 2^32/w: at w = 1440
 * that is ~45.5, about 6 significant bits, and at w = 9000 about 3 --
 * measured on a real game frame against a double-precision pipeline, the
 * old fx_mul(x, fx_recip(w)) put vertices up to 12 px off screen (mean
 * 0.09 px) and depth up to 1510 LSB16 off (mean 158), worst on distant
 * geometry. For |w| >= 2 the divide uses the normalised ~30-bit reciprocal
 * instead; below that (only the w ~ 1 orthographic HUD/skybox reaches here
 * unclipped) fx_recip() is already exact enough. w == 0 divides by 1, as
 * before (the caller rejects those triangles). */
typedef struct { fx_recip_norm_t k; int32_t inv; int8_t norm, neg; } divw_t;

static inline divw_t divw_prep(int32_t w)
{
    divw_t d;
    uint32_t aw = w < 0 ? 0u - (uint32_t)w : (uint32_t)w;
    d.neg = w < 0;
    d.norm = aw >= FX_RECIP_NORM_MIN;
    if (d.norm) d.k = fx_recip_norm(aw);
    else        d.inv = w ? fx_recip(w) : FX_ONE;
    return d;
}

static inline int32_t divw(int32_t x, const divw_t *d)
{
    if (!d->norm) return fx_mul(x, d->inv);
#ifdef GEOM_FX_DIVN_LAST
    int32_t q = fx_div_norm_last(x, d->k.r);   /* divw_prep() just ran fx_recip_norm(w) */
#else
    int32_t q = fx_div_norm(x, d->k);
#endif
    return d->neg ? -q : q;
}

/* c/255 in S15.16: c*257 + (c>>7) is exact at both ends (0 -> 0,
 * 255 -> 65536) and within half an LSB in between. */
static inline int32_t fx_from_u8(uint32_t c8) { return (int32_t)(c8 * 257u + (c8 >> 7)); }

static void load_vertices(const geom_vtx_t *src, int v0, int n)
{
    /* Everything below stays in S15.16 fixed point and converts to float
     * ONCE per stored field. The earlier version kept `float` as the working
     * type and every gm_* op decoded its operands from IEEE bits and
     * re-encoded its result: ~74 conversions per vertex, 64% of all
     * geom-core cycles in a cycle-accurate profile (sim/geom_full). Values
     * are the same up to that float round trip, which only ever DROPPED
     * bits (S15.16 carries up to 31 significant bits, float 24). */
    if (s_mvp_dirty) s_lit_dirty = true;   /* object-space lights follow the modelview */
    update_mvp();
    if (s_vpu_m_gen != s_mvp_gen) {
        for (int i = 0; i < 16; i++) geom_vpar_load_m(i, s_mvp_q[i]);   /* == gm_load_M(s_mvp) */
        s_vpu_m_gen = s_mvp_gen;
    }
    const int32_t sx = s_vp_fx[0], tx = s_vp_fx[1];
    const int32_t sy = s_vp_fx[2], ty = s_vp_fx[3];
    int32_t fogf[VTX_CACHE_SIZE];
    int32_t fog_mul = 0, fog_off = 0, fog_rgb[3] = { 0, 0, 0 };
    if (s_fog_on) {
        fog_mul = s_fog_mul; fog_off = s_fog_off;
        for (int c = 0; c < 3; c++) fog_rgb[c] = s_fog_rgb[c];
    }
#ifdef GEOM_PROJECT_HW
    /* The CFU's PROJECT op does this loop's projection -- column shifts,
     * 1/w, x/w y/w z/w, viewport, depth -- in one instruction, bit-exactly
     * (sim/vpu/tb_setupunit.cpp). The C below stays for |w| < 2^17, where
     * the C divides by w differently; the op then only returns the shifted
     * columns. With fog, only fog's own z / w is left to the C. */
    if ((s_proj_cfg_gen != s_mvp_gen || s_proj_cfg_vp[0] != sx || s_proj_cfg_vp[1] != tx
                     || s_proj_cfg_vp[2] != sy || s_proj_cfg_vp[3] != ty)) {
        geom_proj_set(0, sx); geom_proj_set(1, tx); geom_proj_set(2, sy); geom_proj_set(3, ty);
        geom_proj_set(4, s_mvp_sh[0] | s_mvp_sh[1] << 4 | s_mvp_sh[2] << 8 | s_mvp_sh[3] << 12);
        s_proj_cfg_gen = s_mvp_gen;
        s_proj_cfg_vp[0] = sx; s_proj_cfg_vp[1] = tx; s_proj_cfg_vp[2] = sy; s_proj_cfg_vp[3] = ty;
    }
#endif
    for (int i = 0; i < n; i++) {
        const geom_vtx_t *v = &src[i];
        /* clip = ob * MVP on the CFU; ob is an integer, so its S17.10 form
         * is a shift, not a conversion. */
        geom_vpar_matvec_ob_hp((uint16_t)v->ob[0] | (uint32_t)(uint16_t)v->ob[1] << 16, v->ob[2]);
        gvtx_t *o = &s_vtx[v0 + i];
        int32_t c0, c1, c2, wfx;
        int32_t sx_ = 0, sy_ = 0;       /* o->x, o->y, kept in registers for the flags */
        int projected = 0;
#define VTX_ATTRS() do { \
            if (!s_lighting) \
                { o->r = fx_from_u8(v->cn[0]); o->g = fx_from_u8(v->cn[1]); o->b = fx_from_u8(v->cn[2]); } \
            o->a = fx_from_u8(v->cn[3]); \
            o->s = (int32_t)v->tc[0] * (FX_ONE / 32); \
            o->t = (int32_t)v->tc[1] * (FX_ONE / 32); \
        } while (0)
#ifdef GEOM_PROJECT_HW
        /* PROJECT answers w early and finishes in the background; the
         * vertex's own attributes are converted meanwhile, before the first
         * read of its results waits for it */
        {
            wfx = geom_proj();
            VTX_ATTRS();
            c0 = geom_proj_read(3); c1 = geom_proj_read(4); c2 = geom_proj_read(5);
            uint32_t aw = wfx < 0 ? 0u - (uint32_t)wfx : (uint32_t)wfx;
            if (aw >= FX_RECIP_NORM_MIN) {
                sx_ = o->x = geom_proj_read(0); sy_ = o->y = geom_proj_read(1); o->z = geom_proj_read(2);
                projected = 1;
            }
        }
#else
        {
            VTX_ATTRS();
            c0 = vpar_col_to_fx(geom_vpar_read_out(0), s_mvp_sh[0]);
            c1 = vpar_col_to_fx(geom_vpar_read_out(1), s_mvp_sh[1]);
            c2 = vpar_col_to_fx(geom_vpar_read_out(2), s_mvp_sh[2]);
            wfx = vpar_col_to_fx(geom_vpar_read_out(3), s_mvp_sh[3]);   /* 0 -> 0 */
        }
#endif

#ifdef GEOM_DIAG
        {
        float clip[4] = { geom_vpar_from_fixed(geom_vpar_read_out(0)), geom_vpar_from_fixed(geom_vpar_read_out(1)),
                          geom_vpar_from_fixed(geom_vpar_read_out(2)), geom_vpar_from_fixed(geom_vpar_read_out(3)) };
        extern int g_diag_vtx_dump;
        extern unsigned g_diag_w_neg, g_diag_w_zero, g_diag_w_pos, g_diag_w_negsmall;
        if (clip[3] < 0.f) { g_diag_w_neg++; if (clip[3] > -4.f) g_diag_w_negsmall++; }
        else if (clip[3] == 0.f) g_diag_w_zero++;
        else g_diag_w_pos++;
        if (g_diag_vtx_dump > 0) {
            g_diag_vtx_dump--;
            extern int printf(const char *, ...);
            printf("  vtx ob=(%d,%d,%d) -> clip=(%.2f, %.2f, %.2f, %.4f)\n",
                   v->ob[0], v->ob[1], v->ob[2], clip[0], clip[1], clip[2], clip[3]);
        }
        }
#endif
        divw_t dw;
        int32_t z;
        if (!projected) {
        dw = divw_prep(wfx);
        /* Perspective divide + viewport/screen map STAYS scalar software
         * (S15.16, geom_fixed.c), NOT the CFU's VMUL4/VFMA4 (S17.10) --
         * tried, measured, and reverted: three separate ways this stage
         * touches values S17.10's coarser 10-bit fraction mishandles.
         * (1) invw is often small (order 0.01-0.1), so S17.10's 1/1024
         *     absolute step is a large RELATIVE error on it (~1.6%
         *     measured on a real scene). (2) z's formula depends on
         *     GEOM_DEPTH_K = 65534/65536, a deliberately tiny (~0.00003)
         *     offset from 1.0 kept specifically to stay clear of
         *     the depth clamp at 1.0 (see below) -- S17.10
         *     rounds 0.5*GEOM_DEPTH_K to EXACTLY 0.5, erasing that whole
         *     safety margin. (3) even restricted to JUST x,y (scale
         *     ~100s, translate ~100s, no small-magnitude or near-1.0
         *     operand at all), the per-corner quantization of a quad's 4
         *     vertices -- individually sub-pixel, ~0.02-0.06px each --
         *     was enough to fail a real regression test built to sample
         *     one exact pixel dead centre of a stacked-ortho-layers scene
         *     (T12.4). All three were found on real scene data by
         *     comparing against this exact scalar formula, not by
         *     inspection -- the CFU's own arithmetic is correct (bit-exact
         *     per tb_vpu4dfixed.cpp's 500-trial stress tests), the format
         *     genuinely isn't suited to pixel-precision final output. The
         *     CFU still earns its area on load_vertices()'s matrix-vector
         *     transform above (MATVEC4, large well-conditioned values)
         *     and on lighting (light_vertices() -- normalized
         *     directions/colours, no small-magnitude or near-boundary
         *     operand either). */
        sx_ = o->x = fx_add(fx_mul(divw(c0, &dw), sx), tx);
        sy_ = o->y = fx_sub(ty, fx_mul(divw(c1, &dw), sy));
        /* Depth: the rasterizer takes depth in [0,1) and clamps anything
         * outside it to one extreme, so raw clip-space z (magnitudes like
         * 40-100) would give every fragment the same depth and make the
         * depth test a no-op. Perspective-divide by w for NDC z in [-1,1],
         * map it to [0,1) (zNear 0, zFar 1: there is no glDepthRange), and
         * scale by 65534/65536 to stay clear of the exact 1.0 clamp. */
        int32_t ndc_z = divw(c2, &dw);
        z = fx_mul(fx_add(fx_mul(ndc_z, FX_ONE / 2), FX_ONE / 2), 65534);   /* * GEOM_DEPTH_K */
        o->z = z;
        } else {
            z = o->z;
            if (s_fog_on) dw = divw_prep(wfx);   /* for fog's z / w below */
        }
        o->w = wfx;
        o->cx = c0; o->cy = c1; o->cz = c2;
        o->fl = vtx_screen_flags(sx_, sy_)
              | (geom_w_outside(wfx) || geom_z_outside(z) ? VF_NEAR : 0u)
              | (c2 > wfx ? VF_FAR : 0u) | (wfx == 0 ? VF_W0 : 0u)
              | (wfx > GEOM_ORTHO_W_LO_FX && wfx < GEOM_ORTHO_W_HI_FX ? VF_ORTHO : 0u);

        if (s_fog_on) {
            /* Fog is done here, per vertex: lerp colour toward the fog colour
             * by clamp(ndc_z * fogMul + fogOffset, 0, 1). Applied after the
             * lighting pass below, so only the factor is computed here. */
            int32_t ff = fx_add(fx_mul(divw(z, &dw), fog_mul), fog_off);
            fogf[i] = ff < 0 ? 0 : ff > FX_ONE ? FX_ONE : ff;
        }
    }

    /* Lighting runs as its own pass because it needs a DIFFERENT matrix
     * resident in the CFU (the object-space lights, not the MVP); the next
     * load_vertices() reloads the MVP at its top. */
    if (s_lighting) {
        if (s_lit_dirty) update_obj_lights();
        light_vertices(src, n, &s_vtx[v0]);
    }

    if (s_fog_on)
        for (int i = 0; i < n; i++) {
            gvtx_t *o = &s_vtx[v0 + i];
            const int32_t f = fogf[i];
            o->r = fx_add(fx_mul(fx_sub(fog_rgb[0], o->r), f), o->r);
            o->g = fx_add(fx_mul(fx_sub(fog_rgb[1], o->g), f), o->g);
            o->b = fx_add(fx_mul(fx_sub(fog_rgb[2], o->b), f), o->b);
        }
}

/* ---- triangle emit ------------------------------------------------- */
/* Clip-space w below which a vertex is treated as at/behind the near plane.
 * With the N64 perspective matrix (row-vector, w = -z_eye) geometry in front
 * of the camera has w > 0 and grows with distance; w <= 0 is behind the
 * camera and w in (0, eps] is right on the lens, where the 1/w perspective
 * divide sends screen coords to +/-infinity and the triangle smears across
 * the whole framebuffer. C4: real near-plane clip-and-split -- a triangle
 * with one or two vertices past this plane is clipped against it (producing
 * 1 or 2 new triangles) instead of being dropped whole, so only the actually
 * out-of-view portion is lost. (Earlier version of this comment: "a true
 * clip-and-split is a later refinement" -- this is that refinement.) */
#define GEOM_NEAR_W        4.0f
#define GEOM_NEAR_W_BITS   0x40800000u   /* f2u(4.0f); +ve float bits are monotonic */
#ifdef GEOM_DIAG
unsigned g_diag_tri_in, g_diag_rej_wneg, g_diag_rej_wnear, g_diag_rej_setup, g_diag_emit;
/* Of the triangles that reach the general clip (g_diag_rej_wnear), how many
 * actually need the two w/near planes (insideCount 1 or 2: a real crossing)
 * vs. how many are fully in front of the camera and only poke a screen edge
 * (insideCount==3): the latter's planes 0-1 are skipped now -- see
 * clip_planes_needed() -- so this says how much of the general-clip cost
 * that removed. Host-only measurement, not carried to the target ROM. */
unsigned g_diag_clip_edge_only, g_diag_clip_real;
int g_diag_vtx_dump;
unsigned g_diag_w_neg, g_diag_w_zero, g_diag_w_pos, g_diag_w_negsmall;
int g_diag_mvp_dump;
int g_diag_light_dump;
unsigned g_diag_hud, g_diag_3d; int g_diag_skip_hud;
#endif

/* wbits is the f2u() bit pattern of a vertex's clip-space w. Folds the old
 * two separate checks (absolute "any w<0" reject, "any w<=GEOM_NEAR_W"
 * near-threshold reject) into one per-vertex classification: a negative w
 * already satisfies this (its sign bit is set), so nothing is lost by
 * unifying them -- and the clip-and-split loop below needs a per-vertex
 * inside/outside test anyway. Unsigned bit-pattern compare against
 * GEOM_NEAR_W_BITS is only valid for non-negative floats (monotonic bits),
 * hence the explicit sign-bit check ORed in rather than relied upon. */

/* C5: full frustum clip (near, already above, plus left/right/bottom/top --
 * everything except far, which this pipeline doesn't need: the depth
 * buffer/range handles arbitrarily distant geometry fine, only the
 * near plane and screen edges cause the float blow-ups this file cares
 * about). Without this, a triangle that's entirely in front of the camera
 * (passes the near test) but at a steep off-axis angle -- common wherever
 * the camera sits close to geometry, e.g. a wall right beside Mario, or the
 * ground under his feet -- projects to a screen coordinate thousands of
 * pixels off-screen. geom_triangle.c's edge-function guard band now stops
 * that from corrupting int32 arithmetic, but the *visible* result is still
 * wrong: a huge, valid-but-absurd triangle gets bbox-clamped to the visible
 * frame and paints a giant flat-colored wedge across it (confirmed on
 * real captured game content). The fix is the
 * standard one: clip in clip-space against all 5 planes before the
 * perspective divide, same Sutherland-Hodgman technique C4 already uses for
 * the near plane, just generalized from a fixed 3-vertex triangle to an
 * n-vertex polygon (a triangle clipped against up to 5 planes can grow to
 * at most 3+5=8 vertices).
 *
 * f2ord() turns a float's bit pattern into a monotonic unsigned key (the
 * standard trick: flip all bits if negative, else set the sign bit) so a
 * plain unsigned range compare works across the positive/negative boundary
 * without a native float compare (this core has no FPU -- see this file's
 * other bit-trick comments). Used only for the cheap fast-path pre-check
 * below; the actual clip loop's plane tests go through the VPU (gm_add/
 * gm_sub), matching every other real arithmetic op in this file. */
static inline uint32_t f2ord(uint32_t b) { return (b & 0x80000000u) ? ~b : (b | 0x80000000u); }

/* How far past the screen edge a vertex may lie before its triangle is
 * clipped rather than rasterised as is: the N64's guard band (SM64 runs clip
 * ratio 2, i.e. about one screen either side). The setup's fixed-point edge
 * functions hold to +-724 px absolute (geom_triangle.c EDGE_FIXED_GUARD) and
 * the bounding box is clamped to the screen, so anything up to ~450 px here
 * is exact. On a typical game frame, 64 px sent 472 of 1567 triangles
 * through the clipper; 256 px takes 6% off the frame's geom cycles, 448
 * only 1% more (sim/geom_full). The pictures differ only in where texel
 * boundaries land on the surfaces that were clipped before. */
#ifndef GEOM_SIDE_GUARD_PX
#define GEOM_SIDE_GUARD_PX 256.0f
#endif

/* Recompute the cached screen-space fast-path guard bounds from the current
 * viewport. Called only when GDL_VIEWPORT changes (and once at reset) --
 * never per-triangle -- so this VPU cost is not paid on every triangle. */
/* s_vp_fx: viewport scale/translate in S15.16 (sx tx sy ty), decoded once
 * per GDL_VIEWPORT instead of on every vertex batch. */
static void update_screen_guard(void)
{
    s_vp_fx[0] = s_vp_scale[0]; s_vp_fx[1] = s_vp_trans[0];
    s_vp_fx[2] = s_vp_scale[1]; s_vp_fx[3] = s_vp_trans[1];
    for (int i = 0; i < 2; i++) {
        int32_t t = s_vp_trans[i], sc = s_vp_scale[i], g = (int32_t)GEOM_SIDE_GUARD_PX * FX_ONE;   /* integer px: no IEEE conversion */
        s_screen_guard_lo[i] = fx_sub(fx_sub(t, sc), g);
        s_screen_guard_hi[i] = fx_add(fx_add(t, sc), g);
    }
}

static inline __attribute__((always_inline)) int screen_coord_in_guard(int32_t v, int axis)
{
    return v >= s_screen_guard_lo[axis] && v <= s_screen_guard_hi[axis];
}

#ifndef GEOM_HOST_TEST
/* One side of an upload (GDL_TEXBIND): its size *d, the source step per
 * texel *st (16.16), and the coordinate normalisation *tn when padded. */
static __attribute__((noinline)) void tex_fit(uint32_t n, uint32_t cap, uint32_t *d, uint32_t *st, int32_t *tn)
{
    uint32_t lg = 0;
    while ((1u << lg) < n) lg++;
    *st = 1u << 16;
    if ((1u << lg) > cap) {                      /* resample to the cap */
        lg = 0;
        while ((1u << lg) < cap) lg++;
        *d = cap; *st = n << (16 - lg);
    } else {
        *d = 1u << lg;
        if (*d != n) *tn = FX_ONE >> lg;         /* padded: 1/2^lg (no divider here) */
    }
}
#endif

/* GDL_CCOLOR: the vertex colour becomes (shade or 1) * K, per channel group
 * (the combiner's PRIMITIVE/ENVIRONMENT inputs). Applied to each screen
 * vertex of a triangle once, when it is drawn (emit_triangle, emit_clipped),
 * so K is whatever is current then. */
/* s_cc_set: channels replaced by K; s_cc_mulk: channels multiplied by a K
 * that is not 1.0 (shade * 1.0 == shade: left alone). Set with GDL_CCOLOR. */
static uint32_t s_cc_set, s_cc_mulk;
static inline __attribute__((always_inline)) int32_t cc_ch(int32_t c, int i, uint32_t set, uint32_t mk, int32_t k)
{
    if ((set >> i) & 1u) return k;
    if ((mk >> i) & 1u) return fx_mul(c, k);
    return c;
}
/* the masks and K in locals: a store through o would otherwise make the
 * compiler reload them for every channel (int32_t and uint32_t may alias) */
static inline __attribute__((always_inline)) void cc_vertex(geom_vertex_fx_t *o)
{
    const uint32_t set = s_cc_set, mk = s_cc_mulk;
    const int32_t k0 = s_cc_k[0], k1 = s_cc_k[1], k2 = s_cc_k[2], k3 = s_cc_k[3];
    int32_t r = o->r, g = o->g, b = o->b, a = o->a;
    o->r = cc_ch(r, 0, set, mk, k0); o->g = cc_ch(g, 1, set, mk, k1);
    o->b = cc_ch(b, 2, set, mk, k2); o->a = cc_ch(a, 3, set, mk, k3);
}

static geom_vertex_fx_t make_screen_vertex(const gvtx_t *v, int32_t tns, int32_t tnt)
{
    geom_vertex_fx_t o = { v->x, v->y, v->z, v->w, v->r, v->g, v->b, v->a,
                        /* Normalised [0,1], NOT texel units: GDL_TEXNORM's
                         * factors (tns, tnt) take the game's texel coordinates
                         * to the texture's [0,1) range. */
                        fx_mul(v->s, tns), fx_mul(v->t, tnt), FX_ONE };
    return o;
}

/* Pre-divide clip-space x/y are NOT stored in gvtx_t (load_vertices() only
 * keeps post-divide screen x/y, see its comment) -- reconstruct them
 * algebraically from the stored screen x/y/w and the current viewport by
 * inverting load_vertices()'s own mapping, rather than widening gvtx_t (and
 * paying +8B/vertex x 32-entry cache = +256B RAM on every vertex whether or
 * not it's ever clipped) just for the rare triangle that actually needs it. */
/* S15.16 throughout: decoded once from the gvtx_t floats on entry to the
 * clip (make_clip_vtx) and encoded once on exit (to_screen_vertex). Keeping
 * float as the working type here cost ~60% of the clip path's cycles in
 * IEEE<->fixed conversions (sim/geom_full profile). */
typedef struct { int32_t cx, cy, cz, w, r, g, b, a, s, t; } clip_vtx_t;

/* Per-triangle constants of the clip path, in S15.16. */
typedef struct { int32_t sx, sy, tx, ty, tns, tnt; } clip_ctx_t;

static clip_vtx_t make_clip_vtx(const gvtx_t *v, const clip_ctx_t *k)
{
    clip_vtx_t o;
    int32_t w = v->w;
    (void)k;
    o.cx = v->cx;
    o.cy = v->cy;
    /* Clip-space z, recovered from the stored depth by inverting the NDC->
     * depth map, then re-multiplied by w. It is clip-space z that is linear
     * along an edge and therefore the only thing the clip may interpolate --
     * v->z has already been divided by w, and lerping THAT is wrong. It is
     * silently wrong for an ordinary clip (both endpoints in front, the
     * result merely slightly off) and catastrophically wrong when one
     * endpoint is behind the camera: its w is negative, so its "depth" is a
     * meaningless large value, and the clipped polygon inherits depths
     * nowhere near the surface it is supposed to represent. */
    o.cz = v->cz;
    o.w = w;
    o.r = v->r; o.g = v->g; o.b = v->b; o.a = v->a;
    o.s = v->s; o.t = v->t;
    return o;
}

static int32_t lerp1(int32_t a, int32_t b, int32_t t) { return fx_add(fx_mul(t, fx_sub(b, a)), a); }

static clip_vtx_t lerp_clip_vtx(const clip_vtx_t *p, const clip_vtx_t *q, int32_t t)
{
    clip_vtx_t o;
    o.cx = lerp1(p->cx, q->cx, t);
    o.cy = lerp1(p->cy, q->cy, t);
    o.cz = lerp1(p->cz, q->cz, t);
    o.w  = lerp1(p->w,  q->w,  t);   /* == GEOM_NEAR_W by construction of t (up to rounding) */
    o.r  = lerp1(p->r,  q->r,  t);
    o.g  = lerp1(p->g,  q->g,  t);
    o.b  = lerp1(p->b,  q->b,  t);
    o.a  = lerp1(p->a,  q->a,  t);
    o.s  = lerp1(p->s,  q->s,  t);
    o.t  = lerp1(p->t,  q->t,  t);
    return o;
}

/* Signed distance of a reconstructed clip-space vertex from one of the 5
 * frustum planes; vertex is inside the plane iff the distance is >= 0.
 * Plane 0 is the near plane (kept in the same w<=GEOM_NEAR_W convention as
 * C4, not the more usual z<=-w -- see GEOM_NEAR_W's own comment); 1-4 are
 * the standard clip-space left/right/bottom/top (x/y in [-w,w]). */
static int32_t clip_plane_dist(int plane, const clip_vtx_t *v)
{
    switch (plane) {
    case 0:  return fx_sub(v->w, 4 * FX_ONE);    /* division guard, w >= GEOM_NEAR_W (4) */
    case 1:  return fx_add(v->cz, v->w);         /* real near plane, z>=-w  */
    case 2:  return fx_add(v->cx, v->w);
    case 3:  return fx_sub(v->w, v->cx);
    case 4:  return fx_add(v->cy, v->w);
    case 5:  return fx_sub(v->w, v->cy);
    default: return fx_sub(v->w, v->cz);         /* far plane, z<=w -- N64's F3D clips it too */
    }
}

/* Bit p set when v is outside plane p -- clip_plane_dist(p, v) < 0 for all
 * seven at once, with the same saturating ops so the verdicts are identical. */
static __attribute__((noinline)) uint32_t clip_outcode(const clip_vtx_t *v)
{
    int32_t w = v->w;
    return (fx_sub(w, 4 * FX_ONE) < 0 ? 0x01u : 0u) | (fx_add(v->cz, w) < 0 ? 0x02u : 0u)
         | (fx_add(v->cx, w) < 0 ? 0x04u : 0u) | (fx_sub(w, v->cx) < 0 ? 0x08u : 0u)
         | (fx_add(v->cy, w) < 0 ? 0x10u : 0u) | (fx_sub(w, v->cy) < 0 ? 0x20u : 0u)
         | (fx_sub(w, v->cz) < 0 ? 0x40u : 0u);
}

#define GEOM_MAX_CLIP_VERTS 10 /* 3 initial + at most 1 per plane, 7 planes */

/* One Sutherland-Hodgman pass: clip the n_in-vertex convex polygon `in`
 * against a single plane, writing the result (0..n_in+1 vertices) to `out`
 * (which must not alias `in`). Standard algorithm, generalized from C4's
 * fixed 3-vertex version to an arbitrary vertex count so it can be applied
 * repeatedly, once per plane. */
static int clip_against_plane(int plane, const clip_vtx_t *in, int n_in, clip_vtx_t *out)
{
    if (n_in == 0) return 0;
    int32_t d[GEOM_MAX_CLIP_VERTS];
    for (int i = 0; i < n_in; i++) d[i] = clip_plane_dist(plane, &in[i]);
    int n_out = 0;
    for (int i = 0; i < n_in; i++) {
        int j = i + 1 == n_in ? 0 : i + 1;   /* no divide: the geom core has no divider */
        int inside_i = d[i] >= 0;
        int inside_j = d[j] >= 0;
        if (inside_i) out[n_out++] = in[i];
        if (inside_i != inside_j) {
            /* denom == 0 only if d[i] == d[j], which can't happen when their
             * inside/outside classifications differ, so this is always safe.
             * t = d_i / (d_i - d_j) with the precise divide: fx_recip() is
             * 2^32/x, a handful of significant bits once x is large, and for
             * a triangle several screens wide that put the new vertex over
             * half a pixel inside the plane -- the screen's first or last row
             * went unpainted. */
            divw_t dt = divw_prep(fx_sub(d[i], d[j]));
            int32_t t = divw(d[i], &dt);
            clip_vtx_t o = lerp_clip_vtx(&in[i], &in[j], t);
            /* ...and put it exactly ON the plane, whatever rounding is left. */
            switch (plane) {
            case 0:  o.w  = 4 * FX_ONE; break;
            case 1:  o.cz = -o.w; break;
            case 2:  o.cx = -o.w; break;
            case 3:  o.cx =  o.w; break;
            case 4:  o.cy = -o.w; break;
            case 5:  o.cy =  o.w; break;
            default: o.cz =  o.w; break;
            }
            out[n_out++] = o;
        }
    }
    return n_out;
}

/* Convert a clip-loop vertex back to the screen-space geom_vertex_t
 * geom_triangle_setup() expects -- same formula load_vertices() uses.
 * NOTE: this is called on every vertex the clip loop gathers, which includes
 * *retained original* "inside" vertices (real, varying w) as well as newly
 * lerped intersection vertices (w == GEOM_NEAR_W exactly) -- so this must do
 * a genuine gm_recip(v->w) here, not assume GEOM_NEAR_W_INV. (An earlier
 * version of this function wrongly used the compile-time GEOM_NEAR_W_INV
 * unconditionally, which is only valid for the intersection vertices; caught
 * by hand-checking the 1-inside test scene's expected screen coordinates
 * before it ever ran.) The extra gm_recip only happens on this already-rare
 * clip path, never on the fast (no-clip) path. */
static geom_vertex_fx_t to_screen_vertex(const clip_vtx_t *v, const clip_ctx_t *k)
{
    geom_vertex_fx_t o;
    divw_t dw = divw_prep(v->w);
    o.x = fx_add(fx_mul(divw(v->cx, &dw), k->sx), k->tx);
    o.y = fx_sub(k->ty, fx_mul(divw(v->cy, &dw), k->sy));
    o.z = fx_mul(fx_add(fx_mul(divw(v->cz, &dw), FX_ONE / 2), FX_ONE / 2), 65534);  /* * GEOM_DEPTH_K */
    o.w = v->w;
    o.r = v->r; o.g = v->g; o.b = v->b; o.a = v->a;
    o.s = fx_mul(v->s, k->tns); o.t = fx_mul(v->t, k->tnt); o.q = FX_ONE;
    return o;
}

/* Diagnostic: triangles actually streamed to the rasterizer this frame.
 * Reported back to the game CPU in the done-mailbox so an empty frame can be
 * blamed on either the geom side (count == 0) or the rasterizer (count > 0
 * but nothing on screen). */
uint32_t geom_tris_emitted;

#ifndef GEOM_CRACK_FIX
#define GEOM_CRACK_FIX 1
#endif
/* How far: level L grows each edge by (|a| + |b|) >> (L + 1), i.e. up to
 * 1/4, 1/8 or 1/16 px for L = 1..3 (0: off). Half a pixel (the first cut)
 * drew seams all over Mario, since the grown band extrapolates colour and
 * texture; the cracks are vertex rounding at T-junctions, ~1/64 px, and on the
 * host 1/16 px closes every one in WF's demo (23 -> 0 uncovered pixels) with
 * Mario unchanged. The game CPU can override the level through geom_cfg()'s
 * GEOM_CFG_CRACK field, read once per display list. */
#ifndef GEOM_CRACK_LEVEL
#define GEOM_CRACK_LEVEL 3u      /* (MRDP: on / off only; its amount is fixed at 1/8 px) */
#endif
static unsigned s_crack_level = GEOM_CRACK_LEVEL;

static int s_mrdp_point;   /* the bound texture is point sampled (GDL_TEXBIND_POINT) */

extern int g_geom_cull;    /* geom_triangle.c: GDL_GEOMODE; front faces have edge_fn area > 0 */
#define MRDP_GUARD (23170 << 11)   /* geom_triangle.c EDGE_FIXED_GUARD, in S15.16 */
/* back face (or degenerate) by the sign of edge_fn(v0, v1, v2) =
 * (x2 - x0)(y1 - y0) - (y2 - y0)(x1 - x0) */
static inline int mrdp_cull(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    int64_t a = (int64_t)(x2 - x0) * (y1 - y0) - (int64_t)(y2 - y0) * (x1 - x0);
    return g_geom_cull ? a <= 0 : a == 0;
}
static inline int mrdp_rejects(const geom_vertex_fx_t *v0, const geom_vertex_fx_t *v1,
                               const geom_vertex_fx_t *v2)
{
    const geom_vertex_fx_t *vv[3] = { v0, v1, v2 };
    for (int k = 0; k < 3; k++) {
        int32_t x = vv[k]->x, y = vv[k]->y;
        if (x > MRDP_GUARD || x < -MRDP_GUARD || y > MRDP_GUARD || y < -MRDP_GUARD) return 1;
    }
    return mrdp_cull(v0->x, v0->y, v1->x, v1->y, v2->x, v2->y);
}

/* MRDP (docs/mrdp.md): cull, then state + an N64 triangle command. */
static void mrdp_emit_fx(const geom_vertex_fx_t *v0, const geom_vertex_fx_t *v1,
                         const geom_vertex_fx_t *v2, int ortho_like, const geom_edges_t *e)
{
    /* The cull verdict of geom_triangle_rejects_e() -- the off-screen guard
     * (edge_fn's int32 range, ~724 px), then back faces by the sign of
     * edge_fn's area, or only degenerate ones when culling is off -- on the
     * S15.16 coordinates directly: its conversion to edge space and a second
     * area were ~200 cycles a triangle on top of MRDP's setup. */
    if (!e && mrdp_rejects(v0, v1, v2)) {
#ifndef GEOM_HOST_TEST
        DIAG_INC(3);
#else
        h_diag[3]++;
#endif
        return;
    }
    int want_depth = !ortho_like && (s_rm & GDL_RM_ZCMP);
    int want_decal = want_depth && (s_rm & GDL_RM_DECAL);
    int want_zupd  = want_depth && (s_rm & GDL_RM_ZUPD) && !want_decal;
    int want_blend = (s_rm & GDL_RM_XLU) != 0;
    int tex = s_rs.tmu_enable[0] ? 1 : 0;
    /* the texel's alpha is coverage only when the fragment alpha comes from
     * it (not the lerp mode, not a translucent surface) -- as before */
    int want_alpha = tex && (s_texenv & (1u << 2)) == 0 && !want_blend;
    mrdp_set_state(want_depth, want_zupd, want_decal, want_blend, want_alpha, tex, s_mrdp_point, s_texenv, s_crack_level);

    /* The common case, TRI_R straight into the command (mrdp_setup.h's
     * mrdp_setup_triangle_r() for these flags, without its arrays and loops):
     * snap, sort, area, the exponent check, then the raw fields. */
    if (!want_decal) {
        const int32_t ya = (int32_t)(((uint32_t)v0->y + 0x2000u) & ~0x3FFFu);
        const int32_t yb = (int32_t)(((uint32_t)v1->y + 0x2000u) & ~0x3FFFu);
        const int32_t yc = (int32_t)(((uint32_t)v2->y + 0x2000u) & ~0x3FFFu);
        const geom_vertex_fx_t *p0 = v0, *p1 = v1, *p2 = v2, *pt;
        int32_t y0 = ya, y1 = yb, y2 = yc, yt;
        if (y1 < y0) { pt = p0; p0 = p1; p1 = pt; yt = y0; y0 = y1; y1 = yt; }
        if (y2 < y1) { pt = p1; p1 = p2; p2 = pt; yt = y1; y1 = y2; y2 = yt; }
        if (y1 < y0) { pt = p0; p0 = p1; p1 = pt; yt = y0; y0 = y1; y1 = yt; }
        int32_t dx1 = p1->x - p0->x, dy1 = y1 - y0, dx2 = p2->x - p0->x, dy2 = y2 - y0;
        int64_t area = (int64_t)dx1 * dy2 - (int64_t)dx2 * dy1;
        if (area == 0 || dy2 == 0) return;
        uint32_t dor = (uint32_t)dy2 | (uint32_t)dy1                       /* dy >= 0 */
                     | (dx1 < 0 ? 0u - (uint32_t)dx1 : (uint32_t)dx1)
                     | (dx2 < 0 ? 0u - (uint32_t)dx2 : (uint32_t)dx2);
        if (15 + (63 - mrdp_clz64(area < 0 ? (uint64_t)-area : (uint64_t)area)) >= 33 - mrdp_clz32(dor)) {
            const int z = want_depth || want_zupd;
            const unsigned flags = MRDP_SETUP_SHADE | (tex ? MRDP_SETUP_TEX : 0u) | (z ? MRDP_SETUP_Z : 0u);
            mrdp_w2((MRDP_OP_TRI_R << 24) | (flags << 19), (uint32_t)p0->x);
            mrdp_w2((uint32_t)y0, (uint32_t)p1->x);
            mrdp_w2((uint32_t)y1, (uint32_t)p2->x);
            mrdp_w2((uint32_t)y2, (uint32_t)p0->r);
            mrdp_w2((uint32_t)p1->r, (uint32_t)p2->r);
            mrdp_w2((uint32_t)p0->g, (uint32_t)p1->g);
            mrdp_w2((uint32_t)p2->g, (uint32_t)p0->b);
            mrdp_w2((uint32_t)p1->b, (uint32_t)p2->b);
            mrdp_w2((uint32_t)p0->a, (uint32_t)p1->a);
            if (tex) {
                mrdp_w2((uint32_t)p2->a, (uint32_t)p0->s);
                mrdp_w2((uint32_t)p1->s, (uint32_t)p2->s);
                mrdp_w2((uint32_t)p0->t, (uint32_t)p1->t);
                mrdp_w2((uint32_t)p2->t, (uint32_t)p0->w);
                mrdp_w2((uint32_t)p1->w, (uint32_t)p2->w);
                if (z) { mrdp_w2((uint32_t)p0->z, (uint32_t)p1->z); mrdp_w2((uint32_t)p2->z, 0u); }
            } else if (z) {
                mrdp_w2((uint32_t)p2->a, (uint32_t)p0->z);
                mrdp_w2((uint32_t)p1->z, (uint32_t)p2->z);
            } else {
                mrdp_w2((uint32_t)p2->a, 0u);
            }
            mrdp_go();
#ifndef GEOM_HOST_TEST
            DIAG_INC(4);
            geom_tris_emitted++;
#else
            h_diag[4]++;
#endif
            return;
        }
    }
    /* the raw fields: MRDP converts them (TRI_R, mrdp_tri_r_fields()) --
     * colour x 255, W normalised to w_min / w, S and T by it, Z x 65535/2 */
    const geom_vertex_fx_t *vv[3] = { v0, v1, v2 };
    mrdp_vtx_t mv[3];
    for (int k = 0; k < 3; k++) {
        const geom_vertex_fx_t *v = vv[k];
        mrdp_vtx_t *m = &mv[k];
        m->x = v->x; m->y = v->y;
        m->a[0] = v->r; m->a[1] = v->g; m->a[2] = v->b; m->a[3] = v->a;
        m->a[4] = v->s; m->a[5] = v->t; m->a[6] = v->w;
        m->a[7] = v->z;
    }
    if (mrdp_emit_triangle(mv, tex, want_depth || want_zupd, want_decal)) {
#ifndef GEOM_HOST_TEST
        DIAG_INC(4);
        geom_tris_emitted++;
#else
        h_diag[4]++;
#endif
    }
}

static void emit_setup_and_stream(const geom_vertex_fx_t *v0, const geom_vertex_fx_t *v1,
                                  const geom_vertex_fx_t *v2, int ortho_like,
                                  const geom_edges_t *e)   /* NULL: compute them */
{
    mrdp_emit_fx(v0, v1, v2, ortho_like, e);
}

/* emit_triangle()'s general case: clip against up to 6 planes, then fan.
 * Out of line so emit_triangle()'s common path stays small -- the geom
 * core's I-cache is 4 KB direct-mapped (see geom.ld). */
static __attribute__((noinline)) void emit_clipped(const gvtx_t *a, const gvtx_t *b, const gvtx_t *c,
                                                   int32_t tns, int32_t tnt, int ortho_like, int insideCount)
{
    clip_ctx_t k;
    k.sx = s_vp_fx[0]; k.sy = s_vp_fx[2];
    k.tx = s_vp_fx[1]; k.ty = s_vp_fx[3];
    k.tns = tns;                 k.tnt = tnt;
    clip_vtx_t bufA[GEOM_MAX_CLIP_VERTS], bufB[GEOM_MAX_CLIP_VERTS];
    bufA[0] = make_clip_vtx(a, &k);
    bufA[1] = make_clip_vtx(b, &k);
    bufA[2] = make_clip_vtx(c, &k);
    clip_vtx_t *cur = bufA, *nxt = bufB;
    int n = 3;
    /* Orthographic content (HUD, skybox, transition fades -- anything whose w
     * is ~1) must skip the two w-based planes entirely. Plane 0 is the
     * division guard `w >= GEOM_NEAR_W`, which is 4: with w == 1 all three
     * vertices fail it, the polygon empties, and the triangle vanishes. Plane
     * 1 is the projection's real near plane, equally meaningless without a
     * perspective divide.
     *
     * This is why the caller already skips the near-plane classification for
     * ortho_like -- but skipping it there only kept such triangles OFF the
     * near-clip path while they stayed inside the screen guard band. The
     * moment one pokes outside the viewport it falls through to this general
     * clip and gets erased by the very plane the classification excluded.
     *
     * Symptom: a centred triangle renders at 1x the screen and DISAPPEARS at
     * 2x and every scale beyond. Orthographic skyboxes and HUDs are larger
     * than the viewport, so this erased them outright; a small perspective
     * object in the middle of the view never exercises it. */
    /* Skip the two w/near planes (0 and 1) whenever all 3 input vertices are
     * already known to be on their inside: ortho_like content is exempt from
     * both by definition (see the comment above), and insideCount==3 proves
     * it directly -- that is precisely what "inside" means here (the
     * insideCount computation above tests the exact same two conditions,
     * geom_w_outside()/geom_z_outside(), that clip_plane_dist() computes for
     * planes 0/1; geom_w_outside's `w <= GEOM_NEAR_W` is slightly tighter
     * than plane 0's `w >= GEOM_NEAR_W`, so "not w-outside" always implies
     * "plane 0 distance >= 0", never the reverse -- the direction this needs).
     * A triangle can only reach this general-clip path with insideCount==3
     * by falling through the screen-guard check just above, i.e. it is the
     * "pokes a screen edge but is fully in front of the camera" case (C5);
     * running the w/near passes on it can only ever confirm what is already
     * known and never remove or split a vertex, so skipping them changes no
     * output. Verified, not just argued: test/test_geom.c's "clip-side"
     * scene is exactly this case (insideCount==3, one vertex past a side
     * plane) and its expected bboxes were independently cross-checked
     * against the clip-plane-distance/lerp/fixed-point algorithm in Python
     * -- both before and after this change it reproduces them exactly.
     * Mutation-checked too: forcing first_plane=2 unconditionally (i.e.
     * applying the skip to insideCount<3 as well) breaks clip-1-inside and
     * clip-2-inside while leaving clip-side untouched, confirming the
     * insideCount==3 condition is exactly what makes this safe. */
    int first_plane = (ortho_like || insideCount == 3) ? 2 : 0;
    PROF_T0(t_cl);
    /* Outcodes, as the RSP keeps them per vertex: bit p set when the vertex is
     * outside plane p. All three outside one plane -> nothing survives, and
     * that is 36% of a typical game frame's clipped triangles. A plane no input vertex is
     * outside cannot cut anything either -- every vertex a pass creates lies
     * on an edge between input vertices, so inside that half-space too -- so
     * only the planes some vertex crosses run. Both used to be discovered the
     * slow way, by running every pass. */
    uint32_t oc0 = clip_outcode(&cur[0]), oc1 = clip_outcode(&cur[1]), oc2 = clip_outcode(&cur[2]);
    uint32_t planes = 0x7Fu & ~((1u << first_plane) - 1u);
    uint32_t oc_or = (oc0 | oc1 | oc2) & planes;
    if (oc0 & oc1 & oc2 & planes) n = 0;
    for (int plane = first_plane; plane < 7 && n > 0; plane++) {
        if (!(oc_or & (1u << plane))) continue;
        int n2 = clip_against_plane(plane, cur, n, nxt);
        clip_vtx_t *tmp = cur; cur = nxt; nxt = tmp;
        n = n2;
    }
    PROF_ADD(PR_CLIP, t_cl);
    if (n < 3) {
#ifndef GEOM_HOST_TEST
        DIAG_INC(2);
#else
        { h_diag[2]++; }
#endif
        return;   /* clipped away entirely */
    }

    geom_vertex_fx_t sv[GEOM_MAX_CLIP_VERTS];
    for (int i = 0; i < n; i++) {
        sv[i] = to_screen_vertex(&cur[i], &k);
        if (s_cc_flags & 1u) cc_vertex(&sv[i]);   /* once per vertex: the fan shares them */
    }
    for (int i = 1; i + 1 < n; i++) {
        emit_setup_and_stream(&sv[0], &sv[i], &sv[i + 1], ortho_like, NULL);
    }
}

static void emit_triangle(int i0, int i1, int i2)
{
    const gvtx_t *a = &s_vtx[i0], *b = &s_vtx[i1], *c = &s_vtx[i2];
#ifdef GEOM_DIAG
    g_diag_tri_in++;
#endif

    /* Every per-vertex test below reads gvtx_t.fl (vtx_flags() in
     * load_vertices()): the verdict is one AND and one OR over three words
     * rather than ~30 loads and compares per triangle. */
    const uint32_t fand = a->fl & b->fl & c->fl, forr = a->fl | b->fl | c->fl;

    /* Orthographic/screen-space overlays -- SM64's 2D HUD (health/star/coin,
     * via create_dl_ortho_matrix) and play_transition()'s full-screen fade
     * rectangles -- use a projection where w is always ~1.0 by convention.
     * It carries no camera-distance information, unlike real perspective-
     * projected 3D geometry, so it's never clipped against the near plane. */
    const int ortho_like = (fand & VF_ORTHO) != 0;
#ifdef GEOM_DIAG
    if (ortho_like) { g_diag_hud++; if (g_diag_skip_hud) return; }
    else g_diag_3d++;
#endif

#ifndef GEOM_HOST_TEST
    DIAG_INC(0);
    PROF_CNT(PR_NTRI, 1);
#else
    { h_diag[0]++; }
#endif
    int32_t tns = s_texnorm_s_fx, tnt = s_texnorm_t_fx;
    int insideCount = 3;   /* only "all 3 inside the near planes" or not matters below */
    if (!ortho_like) {
        /* All three behind the camera / inside the w guard / inside the near
         * plane (each a half-space, so the triangle is wholly invisible), or
         * any vertex exactly on the camera plane (w == 0: its screen x/y are
         * meaningless and the clip would fan a degenerate sliver around it)
         * -> nothing to draw, and nothing to clip either. */
        if ((fand & VF_NEAR) || (forr & VF_W0)) {
#ifdef GEOM_DIAG
            g_diag_rej_wneg++;
#endif
#ifndef GEOM_HOST_TEST
            DIAG_INC(1);
#else
            { h_diag[1]++; }
#endif
            return;
        }
        if (forr & VF_NEAR) insideCount = 0;   /* a real near clip: emit_clipped() runs all planes */
    }

    /* Far plane (z <= w), as SM64's Fast3D microcode clips it: a triangle
     * wholly beyond it is dropped, one partly beyond is clipped. The
     * rasterizer would otherwise draw it with its depth clamped to the far
     * value. */
    if (fand & VF_FAR) {
#ifndef GEOM_HOST_TEST
        DIAG_INC(2);
#else
        h_diag[2]++;
#endif
        return;
    }
    int far_any = (forr & VF_FAR) != 0;

    if (insideCount == 3) {
        /* Both near planes are trivially satisfied by all 3 -- their screen
         * x/y are
         * therefore well-behaved (see load_vertices()), so the cheap cached-
         * bound screen-space check below is valid. If all 3 are also
         * comfortably within the viewport (the overwhelmingly common case),
         * take the exact fast path C4 always did: zero clip-related VPU
         * work. Otherwise fall through to the general clip below -- a
         * triangle that's fully in front of the camera but pokes outside the
         * screen edges (C5); that path also skips planes 0-1, see
         * clip_planes_needed(). */
        /* Wholly beyond one edge of the framebuffer: covers no pixel centre.
         * The setup would clamp its bounding box to a one-pixel sliver and
         * emit a triangle that covers nothing; the clip would reduce
         * it to nothing. Either way no pixel changes. (Screen x/y are only
         * meaningful with all three in front of the camera -- this block.) */
        if (fand & VF_OFFSCR) {
#ifndef GEOM_HOST_TEST
            DIAG_INC(3);
#else
            h_diag[3]++;
#endif
            return;
        }
        if (!(forr & VF_NOGUARD) && !far_any) {   /* all six coordinates inside the guard band */
            /* Cull + overflow guard first. They read only x,y, which
             * make_screen_vertex() copies unchanged, so this is the verdict
             * geom_triangle_setup_fx() would reach -- counted the same way. */
            geom_edges_t edges;
            /* (the guard is VF_NOGUARD's; MRDP's setup needs no edge space) */
            if (mrdp_cull(a->x, a->y, b->x, b->y, c->x, c->y)) {
#ifndef GEOM_HOST_TEST
                DIAG_INC(3);
#else
                h_diag[3]++;
#endif
#ifdef GEOM_DIAG
                g_diag_rej_setup++;
#endif
                return;
            }
            geom_vertex_fx_t v0 = make_screen_vertex(a, tns, tnt);
            geom_vertex_fx_t v1 = make_screen_vertex(b, tns, tnt);
            geom_vertex_fx_t v2 = make_screen_vertex(c, tns, tnt);
            if (s_cc_flags & 1u) { cc_vertex(&v0); cc_vertex(&v1); cc_vertex(&v2); }
            emit_setup_and_stream(&v0, &v1, &v2, ortho_like, &edges);
            return;
        }
    }

    /* General case: clip against up to 6 planes (the w guard, the projection's
     * real near plane, then left/right/bottom/top -- C5) via repeated Sutherland-Hodgman passes, each
     * potentially growing the polygon by one vertex. Reconstruct pre-divide
     * clip x/y for the 3 input vertices (cheap, only paid on this rare
     * path) and fan-triangulate whatever polygon survives. */
#ifdef GEOM_DIAG
    g_diag_rej_wnear++;   /* repurposed: counts clip-and-split events, not drops */
    if (insideCount == 3) g_diag_clip_edge_only++; else g_diag_clip_real++;
#endif
    emit_clipped(a, b, c, tns, tnt, ortho_like, insideCount);
}

/* ---- display-list walk ------------------------------------------- */
#if defined(GEOM_HOST_TEST) && defined(GEOM_HOST_TEXC_MODEL)
/* Host model of the geom core's decoded-texture cache (256 entries, started
 * over when full): how many binds decode on the geom core. */
#include <stdio.h>
static struct { uintptr_t src, tlut; uint32_t wh, key; } s_hm[8192];
static uint32_t s_hm_n, s_hm_dl, s_hm_miss, s_hm_binds, s_hm_flushes;
static void host_texc_bind(uintptr_t src, uint32_t wh, uint32_t key, uintptr_t tlut)
{
    static uintptr_t ls; static uint32_t lwh, lkey;     /* the geom core skips a rebind */
    if (src == ls && wh == lwh && key == lkey) return;
    ls = src; lwh = wh; lkey = key;
    s_hm_binds++;
    for (uint32_t i = 0; i < s_hm_n; i++)
        if (s_hm[i].src == src && s_hm[i].wh == wh && s_hm[i].key == key && s_hm[i].tlut == tlut)
            return;
    if (s_hm_n >= 256u) { s_hm_n = 0; s_hm_flushes++; }      /* (the cache flushes) */
    if (s_hm_n < 8192u) { s_hm[s_hm_n].src = src; s_hm[s_hm_n].wh = wh; s_hm[s_hm_n].key = key; s_hm[s_hm_n].tlut = tlut; s_hm_n++; }
    s_hm_miss++;                                          /* a decode */
}
static void host_texc_dl(void)
{
    if (++s_hm_dl % 200u == 0u) {
        fprintf(stderr, "TEXC dl=%u cached=%u flushes=%u binds=%u decodes=%u (last 200 lists)\n",
                s_hm_dl, s_hm_n, s_hm_flushes, s_hm_binds, s_hm_miss);
        s_hm_miss = 0; s_hm_binds = 0;
    }
}
#endif
void geom_run_display_list(const uint32_t *dl)
{
    const uint32_t *pc = dl;
#ifdef GEOM_HOST_TEXC_MODEL
    host_texc_dl();
#endif
    /* the frame's clears (GDL_RAW, or the host simulator's) switched
     * MRDP to fill mode: forget the modes this core last set -- on the host
     * too, or it and the geom core would emit different streams */
    mrdp_state_invalidate();
    /* The texture cache's table is cleared here, at the first display list,
     * not at reset: this core leaves reset before the BIOS initialises SDRAM
     * (a clear then was lost), and SDRAM survives
     * an FPGA reload, so a table a previous run left names stale pages. */
    { static uint8_t s_texc_ready;
      if (!s_texc_ready) { mrdp_texc_reset(); s_texc_ready = 1; } }
#ifndef GEOM_HOST_TEST
#ifdef GEOM_TARGET_INSTRUMENT
    for (int i_ = 0; i_ < PR_COUNT; i_++) s_prof[i_] = 0;
#endif
    {   uint32_t f = (geom_cfg() & GEOM_CFG_CRACK_MASK) >> GEOM_CFG_CRACK_POS;
        s_crack_level = f ? (f - 1u > 3u ? 3u : f - 1u) : GEOM_CRACK_LEVEL; }
    /* The texture cache's table is cleared at the first display list, not in
     * geom_reset(): this core leaves reset with the FPGA, before the BIOS has
     * initialised SDRAM, so a clear at boot was lost and the table held
     * power-up garbage -- every lookup missed and every bind decoded again,
     * 2.1 M cycles a game frame on hardware, while the zero-initialised RTL
     * harness showed the cache working. It must be cleared at all: SDRAM also survives an FPGA reload, so a table a
     * previous run left would name pages that no longer hold its textures. */
    s_cfg_emitted = 0xFFFFFFFFu;   /* ...and may have rewritten the render state */
#ifdef GEOM_TARGET_INSTRUMENT
    const uint32_t t_total = prof_now();
#endif
#endif
    for (;;) {
        uint32_t hdr = *pc++;
        uint8_t  op  = GDL_OP(hdr);
        uint32_t arg = GDL_ARG(hdr);
        if (op == GDL_TRI2) {            /* the commonest command by far: ahead of the switch's compare tree */
            uint32_t w1 = *pc++;
            PROF_T0(t_t);
            emit_triangle((int)((arg >> 16) & 0xFF), (int)((arg >> 8) & 0xFF), (int)(arg & 0xFF));
            emit_triangle((int)((w1 >> 16) & 0xFF), (int)((w1 >> 8) & 0xFF), (int)(w1 & 0xFF));
            PROF_ADD(PR_TRI, t_t);
            continue;
        }
        switch (op) {
        case GDL_END:
#ifndef GEOM_HOST_TEST
            /* Publish the census once per frame, then flush: the game CPU
             * reads it straight out of SDRAM and this core's D-cache is not
             * coherent with it. One flush per frame, on a path that runs once
             * per frame, so it costs nothing measurable. */
#ifdef GEOM_TARGET_INSTRUMENT
            s_diag[12] = (uint32_t) geom_mtx_dropped;
            s_prof[PR_TOTAL] = prof_now() - t_total;
            s_prof[PR_STALL] = 0;                 /* (not measured on the MRDP path) */
            s_prof[PR_WORDS] = 0;
            for (int i_ = 0; i_ < PR_COUNT; i_++) s_diag[13 + i_] = s_prof[i_];
            for (int i_ = 0; i_ < GEOM_DIAG_N; i_++) geom_diag()[i_] = s_diag[i_];
#endif
            geom_dcache_flush();
#endif
            return;

        case GDL_MTX_LOAD: {
            int32_t *dst = (arg == GDL_MTX_TARGET_PROJECTION) ? s_proj : s_mv_stack[s_mv_sp];
            if (!geom_mtx_usable(pc, arg)) { pc += 16; geom_mtx_dropped++; break; }
            int same = 1;
            for (int i = 0; i < 16; i++) if (dst[i] != (int32_t)pc[i]) { same = 0; break; }
            if (!same) {
                memcpy(dst, pc, 16 * sizeof(int32_t)); s_mvp_dirty = true;
                if (arg == GDL_MTX_TARGET_PROJECTION) s_pnz_ok = 0;
            }
            pc += 16;
            break;
        }
        case GDL_MTX_MUL: {
            int32_t m[16];
            if (!geom_mtx_usable(pc, arg)) { pc += 16; geom_mtx_dropped++; break; }
            memcpy(m, pc, 16 * sizeof(int32_t));
            pc += 16;
            int32_t *dst = (arg == GDL_MTX_TARGET_PROJECTION) ? s_proj : s_mv_stack[s_mv_sp];
            gm_mat4_mul_fx(dst, dst, m);
            s_mvp_dirty = true;
            if (arg == GDL_MTX_TARGET_PROJECTION) s_pnz_ok = 0;
            break;
        }
        case GDL_MTX_PUSH:
            if (s_mv_sp + 1 < MTX_STACK_DEPTH) {
                memcpy(s_mv_stack[s_mv_sp + 1], s_mv_stack[s_mv_sp], 16 * sizeof(int32_t));
                s_mv_sp++;
            }
            break;                                /* the same modelview: the MVP stands */
        case GDL_MTX_POP: {
            uint32_t cnt = arg ? arg : 1;
            int sp0 = s_mv_sp;
            while (cnt-- && s_mv_sp > 0) s_mv_sp--;
            if (s_mv_sp != sp0) s_mvp_dirty = true;
            break;
        }
        case GDL_VIEWPORT:
            for (int i = 0; i < 4; i++) s_vp_scale[i] = (int32_t)pc[i];
            for (int i = 0; i < 4; i++) s_vp_trans[i] = (int32_t)pc[4 + i];
            update_screen_guard();
            for (int i_ = 0; i_ < VTX_CACHE_SIZE; i_++)     /* keep gvtx_t.fl's screen bits exact */
                s_vtx[i_].fl = (s_vtx[i_].fl & ~VF_SCREEN) | vtx_screen_flags(s_vtx[i_].x, s_vtx[i_].y);
            pc += 8;
            break;

        case GDL_FBSIZE:
            s_fb_w_fx = (int32_t)((arg & 0xFFFu) << 16);
            s_fb_h_fx = (int32_t)(((arg >> 12) & 0xFFFu) << 16);
            for (int i_ = 0; i_ < VTX_CACHE_SIZE; i_++)
                s_vtx[i_].fl = (s_vtx[i_].fl & ~VF_SCREEN) | vtx_screen_flags(s_vtx[i_].x, s_vtx[i_].y);
            break;

        case GDL_VTX: {
            int n  = (int)((arg >> 8) & 0xFF);
            int v0 = (int)(arg & 0xFF);
            { PROF_T0(t_v); load_vertices((const geom_vtx_t *)pc, v0, n); PROF_ADD(PR_VERT, t_v);
#ifndef GEOM_HOST_TEST
              PROF_CNT(PR_NVERT, (uint32_t)n);
#endif
            }
            pc += (size_t)n * GDL_VTX_WORDS;
            break;
        }
        case GDL_TRI1: {
            PROF_T0(t_t);
            emit_triangle((int)((arg >> 16) & 0xFF), (int)((arg >> 8) & 0xFF), (int)(arg & 0xFF));
            PROF_ADD(PR_TRI, t_t);
            break;
        }
        case GDL_TRI2: {
            uint32_t w1 = *pc++;
            PROF_T0(t_t);
            emit_triangle((int)((arg >> 16) & 0xFF), (int)((arg >> 8) & 0xFF), (int)(arg & 0xFF));
            emit_triangle((int)((w1 >> 16) & 0xFF), (int)((w1 >> 8) & 0xFF), (int)(w1 & 0xFF));
            PROF_ADD(PR_TRI, t_t);
            break;
        }
        case GDL_GEOMODE:
            s_lighting = (arg & GDL_GEOMODE_LIGHTING) != 0;
            if (((arg & GDL_GEOMODE_TEXGEN) != 0) != s_texgen) { s_texgen = !s_texgen; s_lit_dirty = true; }
            geom_set_cull((arg & GDL_GEOMODE_NOCULL) == 0);
            break;
        case GDL_FOG: {
            s_fog_on = (arg & 0x1) != 0;
            uint32_t rgba = *pc++;
            s_fog_rgb[0] = fx_from_u8((rgba >> 24) & 0xFF);
            s_fog_rgb[1] = fx_from_u8((rgba >> 16) & 0xFF);
            s_fog_rgb[2] = fx_from_u8((rgba >> 8)  & 0xFF);
            s_fog_mul = (int32_t)pc[0];
            s_fog_off = (int32_t)pc[1];
            pc += 2;
            break;
        }
        case GDL_CCOLOR: {
            s_cc_flags = arg & 0x7u;
            /* K and the per-channel choice once here, not per vertex drawn */
            s_cc_mul = (arg & 2u ? 7u : 0u) | (arg & 4u ? 8u : 0u);
            { uint32_t rgba = *pc++;
              for (int i = 0; i < 4; i++, rgba <<= 8) s_cc_k[i] = fx_from_u8(rgba >> 24); }
            s_cc_set = ~s_cc_mul & 0xFu; s_cc_mulk = 0;
            for (int i = 0; i < 4; i++) if (((s_cc_mul >> i) & 1u) && s_cc_k[i] != FX_ONE) s_cc_mulk |= 1u << i;
            /* shade * 1.0 on every channel changes nothing: no combiner pass */
            if (s_cc_mul == 0xFu && s_cc_k[0] == FX_ONE && s_cc_k[1] == FX_ONE
                && s_cc_k[2] == FX_ONE && s_cc_k[3] == FX_ONE) s_cc_flags &= ~1u;
            break;
        }
        case GDL_TEXNORM:
            s_texnorm = (int32_t)pc[0];
            pc += 1;
            s_rs.tmu_enable[0] = (s_texnorm != 0);
            if (!s_rs.tmu_enable[0]) { s_texnorm_s_fx = s_texnorm_t_fx = FX_ONE; }
            break;

        case GDL_RENDERMODE:
            s_rm = arg & 0xFu;
#ifdef GEOM_HOST_TEST
            geom_test_rendermode(s_rm);
#endif
            break;

        case GDL_TEXENV:
            s_texenv = arg & 0xFu;
#ifdef GEOM_HOST_TEST
            geom_test_texenv(s_texenv);
#endif
            break;

        case GDL_TEXBIND: {
            PROF_T0(t_tb);
            uint32_t p0 = pc[0];
            uint32_t wh = pc[1];
            int32_t invw = (int32_t)pc[2], invh = (int32_t)pc[3];   /* S15.16 */
            uintptr_t src  = (uintptr_t)pc[4];
            uintptr_t tlut = (uintptr_t)pc[6];
            if (sizeof(uintptr_t) > 4) {           /* host (64-bit) carries a hi half */
                src  |= (uintptr_t)pc[5] << (sizeof(uintptr_t) > 4 ? 32 : 0);
                tlut |= (uintptr_t)pc[7] << (sizeof(uintptr_t) > 4 ? 32 : 0);
            }
            pc += 8;
            s_texnorm_s_fx = invw; s_texnorm_t_fx = invh;
            {   /* decode into an SDRAM staging slot, LOAD_TILE it into TMEM */
                uint32_t fmt = p0 & 0xFu, siz = (p0 >> 4) & 0xFu;
                uint32_t w = wh & 0xFFFFu, h = (wh >> 16) & 0xFFFFu;
                s_mrdp_point = (int)((p0 >> 20) & 1u);
                /* the GDL normalises coordinates by 1/w; MRDP samples in texels */
                s_texnorm_s_fx = invw * (int32_t)w;
                s_texnorm_t_fx = invh * (int32_t)h;
                if (src != 0 && src == s_mrdp_tex_src && wh == s_mrdp_tex_wh && p0 == s_mrdp_tex_p0) {
                    s_rs.tmu_enable[0] = 1;       /* TMEM still holds it */
                    PROF_ADD(PR_TEX, t_tb);
                    break;
                }
                uint32_t line = (w + 3u) >> 2;
                int ok = w && h && w <= 1024u && ((h + 1u) >> 1) * line * 2u <= 1024u
                      && w * h <= MRDP_TEX_SLOT_TEXELS;
                uint32_t addr = 0;
                if (ok) {
                    /* resident: only the LOAD. Else decode into a cache entry,
                     * or (no entry free) into the staging ring */
                    uint32_t key = p0 & 0x000F00FFu;
                    uint64_t s64 = (uint64_t)src, t64 = (uint64_t)tlut;   /* host: 64-bit pointers */
                    uint32_t ksrc = (uint32_t)s64 ^ (uint32_t)(s64 >> 32), ktl = (uint32_t)t64 ^ (uint32_t)(t64 >> 32);
                    int hit;
                    uint32_t *ce = mrdp_texc_find(ksrc, wh, key, ktl, &hit, &addr);
                    if (!hit) {
#ifdef GEOM_HOST_TEST
                        { extern char *getenv(const char *); extern int printf(const char *, ...); static int dbg = -1; if (dbg < 0) dbg = getenv("TEXC_DBG") != 0;
                          if (dbg) printf("TEXMISS ce=%d used=%u fmt=%u siz=%u %ux%u src=%lx\n", ce != 0, s_mrdp_texc_used, fmt, siz, w, h, (unsigned long)src); }
#endif
                        if (!ce) addr = mrdp_tex_slot();
                        ok = geom_texfmt_decode_rgba5551(fmt, siz, (const uint8_t *)mrdp_src_ptr(src), w * h,
                                                         (uint16_t *)mrdp_sdram_ptr(addr));
                        if (ok && ce) mrdp_texc_fill(ce, ksrc, wh, key, ktl);
                        mrdp_store_barrier();           /* the texels before their LOAD */
                    }
                }
                if (ok) {
#ifdef GEOM_HOST_TEST
                    geom_test_mrdp_mem(addr, w * h * 2u);   /* hits too: a captured frame stands alone */
#endif
                    mrdp_texture_bind(addr, w, h, fmt == 3u ? 2u : 0u, (p0 >> 8) & 0xFu, (p0 >> 12) & 0xFu);
                    s_rs.tmu_enable[0] = 1;
                    s_mrdp_tex_src = src; s_mrdp_tex_wh = wh; s_mrdp_tex_p0 = p0;
                } else {
                    s_rs.tmu_enable[0] = 0;
                    s_mrdp_tex_src = 0;
                }
                (void)tlut;
            }
            PROF_ADD(PR_TEX, t_tb);
            break;
        }

        case GDL_LIGHT: {
            if ((arg & 0xFE) == GDL_LIGHT_LOOKAT_X) {      /* gSPLookAt X or Y: a direction only */
                uint32_t dir = pc[1];
                int32_t *L = s_lookat_dir[arg & 1];
                for (int c_ = 0; c_ < 3; c_++) L[c_] = (int32_t)(int8_t)((dir >> (8 * c_)) & 0xFF) * (FX_ONE / 128);
                pc += 2;
                s_lit_dirty = true;
                break;
            }
            int slot      = (int)(arg & 0xFF) & 7;
            int is_amb    = (int)((arg >> 8) & 0x1);
            int num_dir   = (int)((arg >> 16) & 0xFF);
            if (num_dir > 8) num_dir = 8;   /* s_light_dir/s_lit_m hold 8 */
            uint32_t rgb  = *pc++;
            uint32_t dir  = *pc++;
            /* c/255 in S15.16: c*257 + (c>>7) is exact at both ends (0 -> 0,
             * 255 -> 65536) and within half an LSB in between. */
            int32_t *col = is_amb ? s_ambient_q : s_light_col_q[slot];
            for (int c = 0; c < 3; c++) {
                int32_t c8 = (int32_t)((rgb >> (16 - 8 * c)) & 0xFF);
                col[c] = c8 * 257 + (c8 >> 7);
            }
            if (!is_amb) {
                /* Stored as sent: update_obj_lights() renormalises after
                 * moving it into object space, so normalising here too
                 * would be wasted work. */
                for (int c_ = 0; c_ < 3; c_++) {          /* d / 127 in S15.16, nearest */
                    int32_t d = (int8_t)((dir >> (8 * c_)) & 0xFF);
                    /* n / 127 as a multiply-high (no divider on this core, and
                     * -Os would call __divsi3): exact for |n| < 2^24, all 256 d */
                    int32_t n = d * FX_ONE + (d >= 0 ? 63 : -63);
                    uint32_t q = (uint32_t)(((uint64_t)(uint32_t)(n < 0 ? -n : n) * 33818641u) >> 32);
                    s_light_dir[slot][c_] = n < 0 ? -(int32_t)q : (int32_t)q;
                }
            }
            s_lit_dirty = true;
            if (num_dir > s_num_lights) s_num_lights = num_dir;
            s_any_light = 1;
#ifdef GEOM_DIAG
            { extern int g_diag_light_dump; extern int printf(const char*,...);
              if (g_diag_light_dump>0){ g_diag_light_dump--;
                printf("  GDL_LIGHT slot=%d amb=%d num=%d rgb=%06x dir=(%.2f %.2f %.2f)\n",
                       slot,is_amb,num_dir,rgb, is_amb?0.:s_light_dir[slot][0]/65536.,
                       is_amb?0.:s_light_dir[slot][1]/65536., is_amb?0.:s_light_dir[slot][2]/65536.); } }
#endif
            break;
        }

        case GDL_RAW:
            /* frame.c's clears and SWAP, forwarded in GDL order (see
             * geom_gdl.h). The host has no rasterizer FIFO: sw_raster does its
             * own clears, so there it is only skipped. */
            if (arg > GDL_RAW_MAX) return;
            mrdp_out(pc, arg);                    /* frame.c's clears and SYNC FULL */
            pc += arg;
            break;

        case GDL_DL:
            if (s_dl_sp < DL_STACK_DEPTH) s_dl_stack[s_dl_sp++] = pc + 1;
            pc = (const uint32_t *)(uintptr_t)(*pc);
            break;
        case GDL_ENDDL:
            if (s_dl_sp == 0) return;
            pc = s_dl_stack[--s_dl_sp];
            break;

        default:
            return;  /* unknown opcode -- stop rather than run off the rails */
        }
    }
}

#ifdef GEOM_PRINT_FINAL_STATE
int s_dl_sp_dbg(void) { return s_dl_sp; }
int s_fog_on_dbg(void) { return s_fog_on; }
int s_lighting_dbg(void) { return s_lighting; }
int s_mv_sp_dbg(void) { return s_mv_sp; }
int s_num_lights_dbg(void) { return s_num_lights; }
void s_vp_scale_dbg(float *o) { for (int i = 0; i < 4; i++) o[i] = (float)s_vp_scale[i] / 65536.0f; }
void s_vp_trans_dbg(float *o) { for (int i = 0; i < 4; i++) o[i] = (float)s_vp_trans[i] / 65536.0f; }
void s_proj_dbg(float *o) { for (int i = 0; i < 16; i++) o[i] = (float)s_proj[i] / 65536.0f; }
void s_mvp_dbg(float *o) { for (int i = 0; i < 16; i++) o[i] = (float)s_mvp[i] / 65536.0f; }
#endif
