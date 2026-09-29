/* MRDP -- Mirlo's N64-style rasterizer: bit-exact reference model.
 *
 * This file is the contract. The RTL (rtl/mrdp/) must produce the same
 * memory writes, word for word, for the same command stream; the host
 * simulator runs the real game through it. docs/mrdp.md has the design.
 *
 * Commands are 64-bit (two 32-bit words, high word first) with the N64 RDP's
 * opcodes in bits [61:56] and, where a field exists on the N64, the N64's bit
 * position. Addresses are SoC byte addresses (SDRAM is 0x40000000..).
 *
 * Fixed-point formats
 *   YH, YM, YL        s11.2   quarter scanlines
 *   XH, XM, XL        s15.16  x of an edge at an integer scanline: XH and XM
 *                              at floor(YH/4), XL at floor(YM/4)
 *   DxHDy, DxMDy,     s15.16  per scanline
 *   DxLDy
 *   shade R,G,B,A     s15.16  0..255 in the integer part; value at
 *                              (x = XH, y = floor(YH/4)); DaDx per pixel,
 *                              DaDe per scanline along the H edge
 *   S, T              s15.16  texel units; with perspective, S = s * W
 *   W                 s15.16  > 0 (1/w, scaled per triangle: only S/W and
 *                              T/W matter)
 *   Z                 s16.15  depth16 = clamp(Z >> 15, 0, 0xFFFF); larger is
 *                              farther. Z compare passes if depth16 < buffer
 *                              (opaque / xlu / inter) or <= buffer (decal: the
 *                              geom core pulls decals toward the camera)
 *   scissor, rect     u10.2
 *   texrect S,T       s10.5, DsDx/DtDy s5.10
 *
 * Sampling: one sample per pixel, at the pixel centre (x + 0.5, y + 0.5).
 * A pixel is drawn if its centre is in [x_left, x_right) of the scanline and
 * the centre row is in [YH, YL). No coverage / anti-aliasing.
 */
#ifndef MRDP_H
#define MRDP_H

#include <stdint.h>

/* opcodes (bits [61:56] of a command, i.e. [29:24] of its first word) */
enum {
    MRDP_OP_NOP          = 0x00,
    MRDP_OP_TRI          = 0x08,   /* | 4 shade | 2 texture | 1 z */
    MRDP_OP_TEXRECT      = 0x24,
    MRDP_OP_TEXRECT_FLIP = 0x25,
    MRDP_OP_SYNC_LOAD    = 0x26,
    MRDP_OP_SYNC_PIPE    = 0x27,
    MRDP_OP_SYNC_TILE    = 0x28,
    MRDP_OP_SYNC_FULL    = 0x29,
    MRDP_OP_SET_SCISSOR  = 0x2D,
    MRDP_OP_SET_PRIM_Z   = 0x2E,
    MRDP_OP_SET_OTHER    = 0x2F,
    MRDP_OP_SET_TILESIZE = 0x32,
    MRDP_OP_LOAD_TILE    = 0x34,
    MRDP_OP_SET_TILE     = 0x35,
    MRDP_OP_FILL_RECT    = 0x36,
    MRDP_OP_SET_FILL     = 0x37,
    MRDP_OP_SET_FOG      = 0x38,
    MRDP_OP_SET_BLEND    = 0x39,
    MRDP_OP_SET_PRIM     = 0x3A,
    MRDP_OP_SET_ENV      = 0x3B,
    MRDP_OP_SET_COMBINE  = 0x3C,
    MRDP_OP_SET_TIMG     = 0x3D,
    MRDP_OP_SET_ZIMG     = 0x3E,
    MRDP_OP_SET_CIMG     = 0x3F,
};

/* command length in 64-bit words, from the opcode (0 = unknown: skipped) */
int mrdp_cmd_len(uint32_t w0);   /* 64-bit words, from a command's first word */

/* other modes, N64 bit positions: H is the low 24 bits of word 0, L is word 1 */
#define MRDP_H_CYCLE_SHIFT   20        /* 0 1-cycle, 1 2-cycle, 2 copy, 3 fill */
#define MRDP_H_PERSP         (1u << 19)
#define MRDP_H_FILT_SHIFT    12        /* 0 point, 2 bilinear (3-point), 3 average */
#define MRDP_L_ALPHA_CMP     (1u << 0) /* threshold: blend color alpha */
#define MRDP_L_Z_SRC_PRIM    (1u << 2)
#define MRDP_L_AA_EN         (1u << 3) /* the crack grow (or, with MRDP_COVERAGE, the N64's coverage) */
#define MRDP_L_Z_CMP         (1u << 4)
#define MRDP_L_Z_UPD         (1u << 5)
#define MRDP_L_IM_RD         (1u << 6)
#define MRDP_L_ZMODE_SHIFT   10        /* 0 opaque, 1 inter, 2 xlu, 3 decal */
#define MRDP_L_CVG_X_ALPHA   (1u << 12)
#define MRDP_L_ALPHA_CVG_SEL (1u << 13)
#define MRDP_L_FORCE_BL      (1u << 14)
/* blender muxes: [31:30] P0 [29:28] P1 [27:26] A0 [25:24] A1
 *                [23:22] M0 [21:20] M1 [19:18] B0 [17:16] B1 */

/* CVG_X_ALPHA without anti-aliasing: coverage is alpha's top 3 bits, and a
 * pixel with no coverage is not drawn (SM64's TEX_EDGE cut-outs). */
#define MRDP_CVG_ALPHA_MIN 32

typedef struct {
    uint8_t fmt;          /* 0 RGBA5551 (N64 layout: R[15:11] G B A[0]),
                             1 IA88 (I[15:8] A[7:0]), 2 RGBA4444 (R[15:12]..A[3:0]) */
    uint16_t line;        /* texels per TMEM row / 4 (N64: 64-bit words) */
    uint16_t tmem;        /* TMEM address / 4 texels (10 bits: 8 KiB) */
    uint8_t cs, ms, masks, shifts;
    uint8_t ct, mt, maskt, shiftt;
    uint16_t uls, ult, lrs, lrt;  /* u10.2 */
} mrdp_tile_t;

typedef struct {
    uint8_t r, g, b, a;
} mrdp_rgba_t;

/* memory access: 16-bit little-endian halfwords at SoC byte addresses */
typedef uint16_t (*mrdp_rd16_t)(void *ctx, uint32_t addr);
typedef void (*mrdp_wr16_t)(void *ctx, uint32_t addr, uint16_t v);

typedef struct {
    /* command assembly */
    uint32_t cmd[2 * 22];
    int ncmd, need;

    /* state */
    uint32_t other_h, other_l;
    uint32_t combine_hi, combine_lo;
    uint32_t fill;
    mrdp_rgba_t fog, blend, prim, env;
    uint8_t prim_lod_frac;
    uint16_t prim_z;
    uint16_t sc_xh, sc_yh, sc_xl, sc_yl;   /* u10.2 */
    uint32_t cimg, cimg_width;             /* width in pixels */
    uint32_t zimg;
    uint32_t timg, timg_width;             /* width in texels */
    mrdp_tile_t tile[8];
    uint16_t tmem[4][1024];

    /* counters */
    uint32_t sync_count;
    uint32_t load_count;                   /* LOAD TILEs executed */
    uint32_t unknown_ops;
    uint64_t pixels_drawn;
    unsigned grow;                         /* this TRI_R's crack-grow edges (mrdp_grow_mask), else 0 */

    /* debug: report every write to pixel (dbg_x, dbg_y); dbg_x < 0 = off */
    int dbg_x, dbg_y;

    /* memory */
    void *ctx;
    mrdp_rd16_t rd16;
    mrdp_wr16_t wr16;
} mrdp_t;

void mrdp_init(mrdp_t *r, void *ctx, mrdp_rd16_t rd16, mrdp_wr16_t wr16);
/* push one 32-bit command word; a command executes when complete */
void mrdp_push(mrdp_t *r, uint32_t word);

/* building blocks, exported for unit tests and the RTL testbenches */
uint32_t mrdp_rcp(uint32_t w, int *shift);   /* see mrdp.c */
int32_t mrdp_persp(int32_t s, uint32_t w);   /* S/W as s10.5 */

#endif
