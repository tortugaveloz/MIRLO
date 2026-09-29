/* The C spec of TRI_R's setup (lang/c/mrdp/mrdp_setup.h), flattened for
 * tools/mrdp_ucode.py's differential test of the microcode:
 *   cc -O1 -shared -fPIC -I lang/c/mrdp tools/mrdp_ucode_ref.c -o <lib>.so
 * ref_tri_r(w, out): w = the TRI_R command (w[0] .. w[6 + 3n]); out[] =
 *   0 ok, 1 lft, 2 yh, 3 ym, 4 yl (14 bits), 5 dxh, 6 dxm, 7 dxl, 8 xh, 9 xm,
 *   10 xl, 11..14 F0..F3, 15 kx, 16 ky, 17 vexp (e clamped to 0..63),
 *   18..26 S0..S2 T0..T2 W0..W2 as converted in place (texture only),
 *   27 the crack-grow edges (mrdp_grow_mask) */
#include <stdint.h>
#include "mrdp.h"
#include "mrdp_setup.h"

int ref_tri_r(const uint32_t *w, int32_t *out)
{
    unsigned flags = (w[0] >> 19) & 7u;
    int32_t xy[6], kx, ky;
    for (int k = 0; k < 6; k++) xy[k] = (int32_t)w[1 + k];
    mrdp_edges_t E;
    mrdp_fac_t fc;
    for (int i = 0; i < 28; i++) out[i] = 0;
    if (!mrdp_tri_r(xy, &E, &fc, &kx, &ky)) return 0;
    out[0] = 1;
    out[1] = E.lft;
    out[2] = E.yh & 0x3FFF; out[3] = E.ym & 0x3FFF; out[4] = E.yl & 0x3FFF;
    out[5] = E.dxh; out[6] = E.dxm; out[7] = E.dxl;
    out[8] = E.xh; out[9] = E.xm; out[10] = E.xl;
    for (int k = 0; k < 4; k++) out[11 + k] = fc.f[k];
    out[15] = kx; out[16] = ky;
    out[17] = fc.e > 63 ? 63 : fc.e;
    out[27] = (int32_t)mrdp_grow_mask(&E);
    int32_t av[8][3] = { { 0 } };
    const uint32_t *a = w + 7;
    for (int i = 0; i < 8; i++) {
        if (!mrdp_attr_on(flags, i)) continue;
        av[i][0] = (int32_t)a[0]; av[i][1] = (int32_t)a[1]; av[i][2] = (int32_t)a[2];
        a += 3;
    }
    mrdp_tri_r_fields(av, flags);
    for (int k = 0; k < 3; k++) {
        out[18 + k] = av[4][k];
        out[21 + k] = av[5][k];
        out[24 + k] = av[6][k];
    }
    return 1;
}
