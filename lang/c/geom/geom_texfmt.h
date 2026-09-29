/* N64 texture-format decode -> the rasterizer's texels: RGBA16 to RGBA5551
 * (R[15:11] G[10:6] B[5:1] A[0]), the IA formats to RGBA4444 (R[15:12]
 * G[11:8] B[7:4] A[3:0]): 4 bits of alpha keep the soft edges of flames,
 * glows and smoke that a single alpha bit would turn into hard cut-outs. The
 * bind picks the format. The IA comments below speak of the 5551 widths.
 *
 * Header-only, so the same code builds for the geom core (rv32im, no FPU:
 * integer bit ops only) and for host tools.
 *
 * Scope: RGBA16, IA16, IA8 and IA4. CI4/CI8/I4/I8/RGBA32 are not implemented
 * here -- geom_texfmt_decode_rgba5551() returns false for them, and the
 * caller falls back to untextured (Gouraud), as if GDL_TEXBIND had never
 * arrived.
 */
#ifndef GEOM_TEXFMT_H
#define GEOM_TEXFMT_H

#include <stdint.h>
#include <stdbool.h>

/* fmt/siz: N64 G_IM_FMT_ / G_IM_SIZ_ codes, exactly as carried in
 * GDL_TEXBIND's p0 field (geom_pipeline.c's GDL_TEXBIND case / geom_gdl.h).
 * src: raw N64-format texture bytes, big-endian (this port runs
 * NO_SEGMENTED_MEMORY with no asset-time byte-swapping -- the compiled
 * decomp data is bit-for-bit what a real N64 ROM would carry).
 * texel_count: width*height.
 * out: caller-provided buffer of at least texel_count uint16_t.
 * Returns false (leaving *out untouched) for any format/size this decoder
 * doesn't implement -- the caller must treat that as "skip texturing", not
 * as an error to propagate/crash on. */
static inline bool geom_texfmt_decode_rgba5551(uint32_t fmt, uint32_t siz,
    const uint8_t *src, uint32_t texel_count, uint16_t *out)
{
    if (fmt == 0u && siz == 2u) {
        /* RGBA16: on-disk bit layout (R[15:11] G[10:6] B[5:1] A[0]) is
         * already bit-identical to RGBA5551 -- decoding
         * is just the big-endian -> native 16-bit read, no bit-shuffling. */
        for (uint32_t i = 0; i < texel_count; i++) {
            const uint8_t *p = src + 2u * i;
            out[i] = (uint16_t)(((uint32_t)p[0] << 8) | (uint32_t)p[1]);
        }
        return true;
    }

    if (fmt == 3u && siz == 2u) {
        /* IA16: high byte = 8-bit intensity, low byte = 8-bit alpha
         * (matches sw_raster.c's tex_fetch(), already validated against the
         * real reference frame). Expand top 5 bits of intensity into
         * R=G=B, top bit of alpha into the 1-bit alpha channel. */
        for (uint32_t i = 0; i < texel_count; i++) {
            const uint8_t *p = src + 2u * i;
            uint32_t i4 = (uint32_t)p[0] >> 4;   /* 8-bit intensity -> 4-bit */
            uint32_t a4 = (uint32_t)p[1] >> 4;   /* 8-bit alpha -> 4-bit */
            out[i] = (uint16_t)((i4 << 12) | (i4 << 8) | (i4 << 4) | a4);
        }
        return true;
    }

    if (fmt == 3u && siz == 1u) {
        /* IA8: one byte per texel, high nibble = 4-bit intensity, low nibble
         * = 4-bit alpha (sw_raster.c: i = hi/15, a = lo/15). Intensity is
         * widened 4 -> 5 bits by replicating the top bit (0 -> 0, 15 -> 31,
         * monotonic); alpha keeps its top bit for the 1-bit alpha.
         *
         * This exists because of Mario's shadow, a 16x16 IA8 quarter circle
         * mirrored on both axes. It used to reach this function labelled
         * IA16 (a translator took the LOAD size from SETTIMG, and an 8-bit
         * texture is loaded as 16-bit), so it was decoded with twice the
         * stride. Correcting the label alone would have been worse than the
         * bug: there was no IA8 case here, the decode would return false,
         * TMU0 would be disabled and the shadow would be a flat white quad. */
        for (uint32_t i = 0; i < texel_count; i++) {
            uint32_t b  = src[i];
            uint32_t i4 = b >> 4;
            out[i] = (uint16_t)((i4 << 12) | (i4 << 8) | (i4 << 4) | (b & 0xFu));
        }
        return true;
    }

    if (fmt == 3u && siz == 0u) {
        /* IA4: two texels per byte, HIGH nibble first (as in sw_raster.c);
         * each nibble is a 3-bit intensity and a 1-bit alpha in bit 0. */
        for (uint32_t i = 0; i < texel_count; i++) {
            uint32_t b = src[i >> 1];
            uint32_t n = (i & 1u) ? (b & 0xFu) : (b >> 4);
            uint32_t i3 = (n >> 1) & 0x7u;
            uint32_t i4 = (i3 << 1) | (i3 >> 2);  /* 0 -> 0, 7 -> 15 */
            uint32_t a4 = (n & 1u) ? 0xFu : 0u;
            out[i] = (uint16_t)((i4 << 12) | (i4 << 8) | (i4 << 4) | a4);
        }
        return true;
    }

    return false;
}

#endif /* GEOM_TEXFMT_H */
