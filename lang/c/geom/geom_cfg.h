/* The geom core's runtime configuration and diagnostic words: fixed SDRAM
 * addresses the game CPU and the geom core both know (frame.c writes the
 * configuration before the first kick; the geom core writes the counters
 * every frame). Fixed addresses, not linked symbols: a linked symbol moves
 * with every geom firmware rebuild, and a reader holding an old address
 * silently reads zeros. */
#ifndef GEOM_CFG_H
#define GEOM_CFG_H

#include <stdint.h>

/* Configuration flags, written by the game CPU before the first kick. */
#define GEOM_CFG_ADDR          0x41300000u
#define GEOM_CFG_TEX_CAP32     0x1u   /* textures above 32x32 are not bound */
#define GEOM_CFG_TEX_NORING    0x2u   /* one texture staging slot instead of the ring */
#define GEOM_CFG_TEX_NOCACHE   0x4u   /* decode every texture bind again (no decoded-texture cache) */
#define GEOM_CFG_CRACK_POS     8u     /* edge grow ("crack fill"), in 1/16 pixel */
#define GEOM_CFG_CRACK_MASK    0x700u
static inline uint32_t geom_cfg(void) {
    return *(volatile uint32_t *)(uintptr_t)GEOM_CFG_ADDR;
}

/* Per-frame counters, written by the geom core:
 *   [0] triangles entering emit_triangle
 *   [1] rejected: wholly outside a near/guard plane
 *   [2] rejected: the clip emptied the polygon
 *   [3] rejected by triangle setup (offscreen guard, cull, empty bbox)
 *   [4] emitted
 *   [5] of [1], all three vertices failing the W test
 *   [6] of [1], all three failing the depth test
 *   [7] vertices seen
 *   [12] matrices the degenerate-modelview guard dropped
 *   [13..24] the per-frame cycle profile (geom_pipeline.c PR_*) */
#define GEOM_DIAG_ADDR         0x41300010u
#define GEOM_DIAG_N            25
static inline volatile uint32_t *geom_diag(void) {
    return (volatile uint32_t *)(uintptr_t)GEOM_DIAG_ADDR;
}

#endif
