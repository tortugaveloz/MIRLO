/* A spinning cube: 8 vertices, 12 triangles, one flat colour per face,
 * perspective projection and the depth test. The cube turns on two axes at
 * different speeds, so every pose comes up sooner or later -- face-on (the
 * most pixels to fill) as well as edge-on.
 *
 * Builds its GDL directly (gdl_build.h) every frame; see demo_main.c for the
 * smallest program of this kind.
 */
#include <stdint.h>
#include <stdio.h>
#include <generated/csr.h>
#include <generated/soc.h>

#include "frame.h"
#include "gdl_build.h"

/* sx,cx = sin/cos of the rotation about X; sy,cy = about Y.
 * mv = Ry * Rx * translate(0, 0, eye_z), row-vector convention. */
static void set_camera(gdl_cur_t *c, float eye_z, float sx, float cx, float sy, float cy)
{
    static const float proj[16] = { 1.6f,0,0,0,  0,2.0f,0,0,  0,0,-1.0f,-1.0f,  0,0,-40.0f,0 };
    float mv[16] = {
         cy,      sx*sy,   cx*sy,  0,
         0,       cx,      -sx,    0,
        -sy,      sx*cy,   cx*cy,  0,
         0,       0,       eye_z,  1,
    };
    const float vp[4] = { VIDEO_FRAMEBUFFER_HRES * 0.5f, VIDEO_FRAMEBUFFER_VRES * 0.5f, 0, 0 };
    gdl_mtx_load(c, GDL_MTX_TARGET_PROJECTION, proj);
    gdl_mtx_load(c, GDL_MTX_TARGET_MODELVIEW, mv);
    gdl_viewport(c, vp, vp);
    gdl_geomode(c, 0);          /* no lighting, back faces culled */
    gdl_texnorm(c, 0.0f);       /* no texture */
}

static geom_vtx_t v(int16_t x, int16_t y, int16_t z, uint8_t r, uint8_t g, uint8_t b)
{
    geom_vtx_t o = {{ x, y, z }, 0, {0,0}, { r, g, b, 255 }};
    return o;
}

/* Faces wound counter-clockwise seen from outside. */
static void build_cube(gdl_cur_t *c)
{
    enum { H = 20 };
    geom_vtx_t front[4]  = { v(-H,-H, H, 220,40,40),  v( H,-H, H, 220,40,40),  v( H, H, H, 220,40,40),  v(-H, H, H, 220,40,40) };
    geom_vtx_t back[4]   = { v( H,-H,-H, 40,220,220), v(-H,-H,-H, 40,220,220), v(-H, H,-H, 40,220,220), v( H, H,-H, 40,220,220) };
    geom_vtx_t right[4]  = { v( H,-H, H, 40,220,40),  v( H,-H,-H, 40,220,40),  v( H, H,-H, 40,220,40),  v( H, H, H, 40,220,40) };
    geom_vtx_t left[4]   = { v(-H,-H,-H, 220,40,220), v(-H,-H, H, 220,40,220), v(-H, H, H, 220,40,220), v(-H, H,-H, 220,40,220) };
    geom_vtx_t top[4]    = { v(-H, H,-H, 220,220,40), v(-H, H, H, 220,220,40), v( H, H, H, 220,220,40), v( H, H,-H, 220,220,40) };
    geom_vtx_t bottom[4] = { v(-H,-H,-H, 40,40,220),  v( H,-H,-H, 40,40,220),  v( H,-H, H, 40,40,220),  v(-H,-H, H, 40,40,220) };
    geom_vtx_t *faces[6] = { front, back, right, left, top, bottom };
    for (int f = 0; f < 6; f++) {
        gdl_vtx(c, 0, faces[f], 4);
        gdl_tri1(c, 0, 1, 2);
        gdl_tri1(c, 0, 2, 3);
    }
}

int main(void)
{
    printf("Mirlo: spinning cube\n");
    frame_init();

    /* Incremental rotation by a fixed small angle: sin/cos of the step are
     * constants, so this needs no trig library. Renormalised now and then,
     * because repeated rotation slowly loses unit length in float. */
    float sx = 0, cx = 1, sy = 0, cy = 1;
    for (uint32_t n = 1;; n++) {
        gdl_cur_t c;
        frame_begin(&c);
        set_camera(&c, -100.0f, sx, cx, sy, cy);
        build_cube(&c);
        frame_submit(&c);

        const float ds = 0.0314f, dc = 0.9995f;      /* ~1.8 degrees */
        float nsx = sx * dc + cx * ds, ncx = cx * dc - sx * ds;
        sx = nsx; cx = ncx;
        const float ds2 = 0.0105f, dc2 = 0.99994f;   /* a third of that */
        float nsy = sy * dc2 + cy * ds2, ncy = cy * dc2 - sy * ds2;
        sy = nsy; cy = ncy;
        if ((n & 255) == 0) {                        /* one Newton step of 1/sqrt */
            float ix = 1.5f - 0.5f * (sx * sx + cx * cx), iy = 1.5f - 0.5f * (sy * sy + cy * cy);
            sx *= ix; cx *= ix; sy *= iy; cy *= iy;
        }
    }
}
