/* Synthetic GDL generator for check_host_vs_rtl.sh and cycle measurements.
 *
 *   mkgdl lit nlights tris fog [tex spread seed]  > gdl.bin
 *
 * 8 batches of 32 vertices under a fixed projection/modelview; `tris` adds
 * 10 triangles per batch; `tex` binds an 8x8 RGBA16 texture whose texels
 * live after GDL_END (addressed at the sim's GDL load address 0x41000000);
 * `spread` scales vertex positions up so triangles cross the near plane and
 * screen edges (exercising the clip paths); `seed` picks the random scene. */
#include "geom_gdl.h"
#include "gdl_build.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#define GDL_LOAD_ADDR 0x41000000u
int main(int argc, char **argv) {
    int lit = atoi(argv[1]), nlights = atoi(argv[2]), tris = argc > 3 ? atoi(argv[3]) : 0,
        fog = argc > 4 ? atoi(argv[4]) : 0, tex = argc > 5 ? atoi(argv[5]) : 0;
    double spread = argc > 6 ? atof(argv[6]) : 1.0;
    srand(argc > 7 ? (unsigned)atoi(argv[7]) : 7u);
    static uint32_t gdl[65536]; gdl_cur_t c; gdl_begin(&c, gdl, 65536);
    float proj[16] = {1.2f,0,0,0, 0,1.6f,0,0, 0,0,-1.01f,-1, 0,0,-40.0f,0};
    float mv[16] = {0.8f,0.1f,-0.59f,0, 0,0.98f,0.17f,0, 0.6f,-0.13f,0.79f,0, 5,-10,-400,1};
    gdl_mtx_load(&c, GDL_MTX_TARGET_PROJECTION, proj);
    gdl_mtx_load(&c, GDL_MTX_TARGET_MODELVIEW, mv);
    gdl_light(&c, 0, 1, nlights, 0x303030, 0, 0, 0);
    for (int k = 0; k < nlights; k++) gdl_light(&c, k, 0, nlights, 0xC0A080, 40 + 10*k, 80, 60 - 20*k);
    gdl_geomode(&c, lit);
    if (fog) gdl_fog(&c, 1, 0x80604000u, 0.7f, 0.2f);
    uint32_t *texref = NULL;
    if (tex) {
        gdl_texbind(&c, 0 /*RGBA*/, 2 /*16b*/, 0, 0, 0, 8, 8, (const void *)(uintptr_t)0, NULL);
        texref = c.p - 4;                      /* src low word, patched below */
        gdl_texnorm(&c, 1.0f / 8.0f);
    }
    for (int b = 0; b < 8; b++) {
        geom_vtx_t v[32];
        for (int i = 0; i < 32; i++) {
            for (int j = 0; j < 3; j++) {
                double p = (rand() % 200 - 100) * spread;
                v[i].ob[j] = (int16_t)(p > 32767 ? 32767 : p < -32768 ? -32768 : p);
            }
            double nx = rand()%255-127, ny = rand()%255-127, nz = rand()%255-127, l = sqrt(nx*nx+ny*ny+nz*nz)+1e-9;
            if (lit) {
                v[i].cn[0] = (uint8_t)(int8_t)lrint(nx/l*127); v[i].cn[1] = (uint8_t)(int8_t)lrint(ny/l*127);
                v[i].cn[2] = (uint8_t)(int8_t)lrint(nz/l*127);
            } else {
                v[i].cn[0] = (uint8_t)rand(); v[i].cn[1] = (uint8_t)rand(); v[i].cn[2] = (uint8_t)rand();
            }
            v[i].cn[3] = (uint8_t)(128 + rand() % 128);
            v[i].tc[0] = (int16_t)(rand() % 512 - 128); v[i].tc[1] = (int16_t)(rand() % 512 - 128);
        }
        gdl_vtx(&c, 0, v, 32);
        if (tris) for (int t = 0; t + 2 < 32; t += 3) gdl_tri1(&c, t, t + 1, t + 2);
    }
    gdl_end(&c);
    if (tex) {
        uint32_t off = (uint32_t)(c.p - gdl);
        texref[0] = GDL_LOAD_ADDR + off * 4u; texref[1] = 0;
        for (int i = 0; i < 32; i++) gdl_w(&c, ((uint32_t)rand() << 1) | 0x00010001u);  /* 64 texels */
    }
    fwrite(gdl, 4, (size_t)(c.p - gdl), stdout);
    return 0;
}
