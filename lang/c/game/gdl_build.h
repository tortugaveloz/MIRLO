/* Game-CPU helpers for building a GDL display list (the format the geometry
 * core walks -- see lang/c/geom/geom_gdl.h). Writes into a caller-provided
 * word buffer via a cursor. */
#ifndef GAME_GDL_BUILD_H
#define GAME_GDL_BUILD_H

#include <stdint.h>
#include <string.h>
#include "../geom/geom_gdl.h"

typedef struct { uint32_t *p; uint32_t *end; } gdl_cur_t;

static inline void gdl_begin(gdl_cur_t *c, uint32_t *buf, uint32_t words) { c->p = buf; c->end = buf + words; }
static inline void gdl_w(gdl_cur_t *c, uint32_t w)  { if (c->p < c->end) *c->p = w; c->p++; }
/* Every non-integer GDL payload word is S15.16 (see geom_gdl.h): the geom
 * core has no FPU and works in fixed point, and SM64's own matrices already
 * are S15.16. A float is converted HERE, on the CPU that has an FPU:
 * nearest, halves away from zero, saturated to int32. */
static inline int32_t gdl_fx(float f) {
    float v = f * 65536.0f;
    if (!(v == v)) return 0;                                   /* NaN */
    if (v >=  2147483520.0f) return 0x7FFFFFFF;                /* largest float < 2^31 */
    if (v <= -2147483648.0f) return (int32_t)0x80000000;
    return (int32_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}
static inline void gdl_f(gdl_cur_t *c, float f)     { gdl_w(c, (uint32_t)gdl_fx(f)); }

static inline void gdl_mtx_load(gdl_cur_t *c, int target, const float m[16]) {
    gdl_w(c, GDL_HDR(GDL_MTX_LOAD, (uint32_t)target)); for (int i = 0; i < 16; i++) gdl_f(c, m[i]);
}
static inline void gdl_mtx_mul(gdl_cur_t *c, int target, const float m[16]) {
    gdl_w(c, GDL_HDR(GDL_MTX_MUL, (uint32_t)target)); for (int i = 0; i < 16; i++) gdl_f(c, m[i]);
}
/* S15.16 matrices as-is (e.g. a decoded N64 Mtx) -- no float at all */
static inline void gdl_mtx_load_fx(gdl_cur_t *c, int target, const int32_t m[16]) {
    gdl_w(c, GDL_HDR(GDL_MTX_LOAD, (uint32_t)target)); for (int i = 0; i < 16; i++) gdl_w(c, (uint32_t)m[i]);
}
static inline void gdl_mtx_mul_fx(gdl_cur_t *c, int target, const int32_t m[16]) {
    gdl_w(c, GDL_HDR(GDL_MTX_MUL, (uint32_t)target)); for (int i = 0; i < 16; i++) gdl_w(c, (uint32_t)m[i]);
}
static inline void gdl_mtx_push(gdl_cur_t *c)          { gdl_w(c, GDL_HDR(GDL_MTX_PUSH, 0)); }
static inline void gdl_mtx_pop(gdl_cur_t *c, int n)    { gdl_w(c, GDL_HDR(GDL_MTX_POP, (uint32_t)n)); }
static inline void gdl_viewport(gdl_cur_t *c, const float scale[4], const float trans[4]) {
    gdl_w(c, GDL_HDR(GDL_VIEWPORT, 0));
    for (int i = 0; i < 4; i++) gdl_f(c, scale[i]);
    for (int i = 0; i < 4; i++) gdl_f(c, trans[i]);
}
/* the same in S15.16, without a float (the geom core's translator, geom_f3d.c) */
static inline void gdl_viewport_fx(gdl_cur_t *c, const int32_t scale[4], const int32_t trans[4]) {
    gdl_w(c, GDL_HDR(GDL_VIEWPORT, 0));
    for (int i = 0; i < 4; i++) gdl_w(c, (uint32_t)scale[i]);
    for (int i = 0; i < 4; i++) gdl_w(c, (uint32_t)trans[i]);
}
static inline void gdl_fbsize(gdl_cur_t *c, unsigned hres, unsigned vres) {
    gdl_w(c, GDL_HDR(GDL_FBSIZE, (hres & 0xFFFu) | (vres & 0xFFFu) << 12));
}
/* flags: GDL_GEOMODE_* (a plain 0/1 is lighting off/on, culling on) */
static inline void gdl_geomode(gdl_cur_t *c, int flags) { gdl_w(c, GDL_HDR(GDL_GEOMODE, (uint32_t)flags & 0x7u)); }
/* mode: GDL_TE_RGB_* | GDL_TE_A_* (geom_gdl.h) */
static inline void gdl_texenv(gdl_cur_t *c, uint32_t mode) { gdl_w(c, GDL_HDR(GDL_TEXENV, mode & 0xFu)); }
static inline void gdl_ccolor(gdl_cur_t *c, uint32_t flags, uint32_t rgba) {
    gdl_w(c, GDL_HDR(GDL_CCOLOR, flags & 0x7u)); gdl_w(c, rgba);
}
static inline void gdl_texnorm(gdl_cur_t *c, float inv_tex_size) { gdl_w(c, GDL_HDR(GDL_TEXNORM, 0)); gdl_f(c, inv_tex_size); }
/* n / d, without a divide instruction (the geom core has none; a texture
 * bind is rare): a shift for a power of two, else shift-and-subtract */
static inline uint32_t gdl_udiv(uint32_t n, uint32_t d) {
    if (d == 0u) return 0u;
    if ((d & (d - 1u)) == 0u) { while (d > 1u) { n >>= 1; d >>= 1; } return n; }
    uint32_t q = 0u, r = 0u;
    for (int i = 31; i >= 0; i--) {
        r = r << 1 | ((n >> i) & 1u);
        if (r >= d) { r -= d; q |= 1u << i; }
    }
    return q;
}
/* 1/size in S15.16, rounded to nearest (0: texturing off) */
static inline void gdl_texnorm_size(gdl_cur_t *c, uint32_t size) {
    gdl_w(c, GDL_HDR(GDL_TEXNORM, 0)); gdl_w(c, size ? gdl_udiv(65536u + size / 2u, size) : 0u);
}
static inline void gdl_fog(gdl_cur_t *c, int on, uint32_t rgba8, float mul, float off) {
    gdl_w(c, GDL_HDR(GDL_FOG, on ? 1 : 0)); gdl_w(c, rgba8); gdl_f(c, mul); gdl_f(c, off);
}
/* sscale/tscale are gsSPTexture's scale factors (1.0 = 0xFFFF). The vertices' s,t are in
 * units of texels/32 times that scale, so it folds into the normalisation. */
/* palette may carry GDL_TEXBIND_POINT (bit 4 of it lands in p0 bit 20): sample
 * without filtering, as the N64 does in copy mode and with G_TF_POINT. */
#define GDL_TEXBIND_POINT 0x10
static inline void gdl_texbind_scaled(gdl_cur_t *c, int fmt, int siz, int cms, int cmt, int palette,
                               int w, int h, const void *src, const void *tlut,
                               float sscale, float tscale) {
    float invw = w > 0 ? sscale / (float)w : 0.0f;
    float invh = h > 0 ? tscale / (float)h : 0.0f;
    uint64_t sp = (uint64_t)(uintptr_t)src, tp = (uint64_t)(uintptr_t)tlut;
    gdl_w(c, GDL_HDR(GDL_TEXBIND, 0));
    gdl_w(c, ((uint32_t)(fmt & 0xF)) | ((uint32_t)(siz & 0xF) << 4)
           | ((uint32_t)(cms & 0xF) << 8) | ((uint32_t)(cmt & 0xF) << 12)
           | ((uint32_t)(palette & 0x1F) << 16));
    gdl_w(c, ((uint32_t)(w & 0xFFFF)) | ((uint32_t)(h & 0xFFFF) << 16));
    gdl_f(c, invw); gdl_f(c, invh);
    gdl_w(c, (uint32_t)sp); gdl_w(c, (uint32_t)(sp >> 32));
    gdl_w(c, (uint32_t)tp); gdl_w(c, (uint32_t)(tp >> 32));
}
/* the same with gsSPTexture's scales in S15.16 (0x10000 = 1.0): integers only */
static inline void gdl_texbind_scaled_fx(gdl_cur_t *c, int fmt, int siz, int cms, int cmt, int palette,
                                         int w, int h, const void *src, const void *tlut,
                                         uint32_t sscale, uint32_t tscale) {
    uint32_t invw = w > 0 ? gdl_udiv(sscale + (uint32_t)w / 2u, (uint32_t)w) : 0u;
    uint32_t invh = h > 0 ? gdl_udiv(tscale + (uint32_t)h / 2u, (uint32_t)h) : 0u;
    uint64_t sp = (uint64_t)(uintptr_t)src, tp = (uint64_t)(uintptr_t)tlut;
    gdl_w(c, GDL_HDR(GDL_TEXBIND, 0));
    gdl_w(c, ((uint32_t)(fmt & 0xF)) | ((uint32_t)(siz & 0xF) << 4)
           | ((uint32_t)(cms & 0xF) << 8) | ((uint32_t)(cmt & 0xF) << 12)
           | ((uint32_t)(palette & 0x1F) << 16));
    gdl_w(c, ((uint32_t)(w & 0xFFFF)) | ((uint32_t)(h & 0xFFFF) << 16));
    gdl_w(c, invw); gdl_w(c, invh);
    gdl_w(c, (uint32_t)sp); gdl_w(c, (uint32_t)(sp >> 32));
    gdl_w(c, (uint32_t)tp); gdl_w(c, (uint32_t)(tp >> 32));
}
static inline void gdl_rendermode(gdl_cur_t *c, uint32_t rm_flags) {
    gdl_w(c, GDL_HDR(GDL_RENDERMODE, rm_flags & 0xFu));
}
static inline void gdl_texbind(gdl_cur_t *c, int fmt, int siz, int cms, int cmt, int palette,
                               int w, int h, const void *src, const void *tlut) {
    gdl_texbind_scaled(c, fmt, siz, cms, cmt, palette, w, h, src, tlut, 1.0f, 1.0f);
}
static inline void gdl_light(gdl_cur_t *c, int slot, int is_ambient, int num_dir,
                             uint32_t rgb, int8_t dx, int8_t dy, int8_t dz) {
    gdl_w(c, GDL_HDR(GDL_LIGHT, ((uint32_t)(slot & 0xFF))
                              | ((uint32_t)(is_ambient ? 1 : 0) << 8)
                              | ((uint32_t)(num_dir & 0xFF) << 16)));
    gdl_w(c, rgb & 0x00FFFFFFu);
    gdl_w(c, ((uint32_t)(uint8_t)dx) | ((uint32_t)(uint8_t)dy << 8) | ((uint32_t)(uint8_t)dz << 16));
}
static inline void gdl_vtx(gdl_cur_t *c, int v0, const geom_vtx_t *v, int n) {
    gdl_w(c, GDL_HDR(GDL_VTX, ((uint32_t)n << 8) | (uint32_t)v0));
    for (int i = 0; i < n; i++) { if (c->p + GDL_VTX_WORDS <= c->end) memcpy(c->p, &v[i], sizeof(geom_vtx_t)); c->p += GDL_VTX_WORDS; }
}
static inline void gdl_tri1(gdl_cur_t *c, int a, int b, int d) {
    gdl_w(c, GDL_HDR(GDL_TRI1, ((uint32_t)a << 16) | ((uint32_t)b << 8) | (uint32_t)d));
}
static inline void gdl_tri2(gdl_cur_t *c, int a, int b, int d, int e, int f, int g) {
    gdl_w(c, GDL_HDR(GDL_TRI2, ((uint32_t)a << 16) | ((uint32_t)b << 8) | (uint32_t)d));
    gdl_w(c, ((uint32_t)e << 16) | ((uint32_t)f << 8) | (uint32_t)g);
}
static inline void gdl_end(gdl_cur_t *c) { gdl_w(c, GDL_HDR(GDL_END, 0)); }
/* An F3DEX2 display list for the geom core to translate (GDL_F3D, geom_gdl.h).
 * The translation goes into the rest of the list's buffer, 16 words past this
 * command: room for what the list still gets after it (frame.c's SYNC FULL
 * and GDL_END). */
#define GDL_F3D_GAP 16u
static inline void gdl_f3d(gdl_cur_t *c, const void *root, uint32_t *clear_word,
                           const void *static_lo, const void *static_hi, void *arena, uint32_t arena_bytes) {
    uint32_t *out = c->p + 1 + GDL_F3D_WORDS + GDL_F3D_GAP;
    uint32_t words = out < c->end ? (uint32_t)(c->end - out) : 0u;
    gdl_w(c, GDL_HDR(GDL_F3D, GDL_F3D_WORDS));
    gdl_w(c, (uint32_t)(uintptr_t)root); gdl_w(c, (uint32_t)(uintptr_t)clear_word);
    gdl_w(c, (uint32_t)(uintptr_t)out); gdl_w(c, words);
    gdl_w(c, (uint32_t)(uintptr_t)static_lo); gdl_w(c, (uint32_t)(uintptr_t)static_hi);
    gdl_w(c, (uint32_t)(uintptr_t)arena); gdl_w(c, arena_bytes);
}

/* words actually written (may exceed the buffer -- check for overflow). */
static inline uint32_t gdl_used(const gdl_cur_t *c, const uint32_t *buf) { return (uint32_t)(c->p - buf); }

#endif /* GAME_GDL_BUILD_H */
