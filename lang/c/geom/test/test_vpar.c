/* Host check for lang/c/geom/geom_vpar.h's host-side model (the same
 * round-half-up/saturate arithmetic as rtl/vpu/Vpu4DFixed.v, verified there
 * against real hardware via sim/vpu/tb_vpu4dfixed.cpp). Checks the FIRMWARE
 * API (geom_vpar_matvec_f, the float in/float out wrapper load_vertices()
 * would actually call) against a plain double reference, not the raw
 * function-id calls tb_vpu4dfixed.cpp drives directly -- this is the layer
 * above that one, not a duplicate of it.
 *
 *   gcc -O2 -Wall -o /tmp/test_vpar test_vpar.c -lm && /tmp/test_vpar
 */
#define GEOM_HOST_TEST
#include <stdio.h>
#include <math.h>
#include "../geom_vpar.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    /* identity */
    {
        float I[16] = {0}; I[0] = I[5] = I[10] = I[15] = 1.0f;
        float v[4] = { 1.5f, -2.0f, 3.25f, 1.0f };
        float out[4];
        geom_vpar_matvec_f(I, v, out);
        for (int i = 0; i < 4; i++)
            CHECK(fabsf(out[i] - v[i]) < 0.002f, "identity[%d]: got %.5f want %.5f", i, out[i], v[i]);
        if (fails == 0) printf("  ok   identity: (%.3f,%.3f,%.3f,%.3f)\n", out[0], out[1], out[2], out[3]);
    }

    /* a realistic MVP-shaped transform, cross-checked against a plain
     * double computation of the same math (not the fixed-point one --
     * this is the precision check, matching what fixedpoint_matvec_precision.c
     * already established, just exercised through the actual firmware API). */
    {
        double ax=0.4, ay=1.1, az=-0.7, scale=1.7;
        double sx=sin(ax), cx=cos(ax), sy=sin(ay), cy=cos(ay), sz=sin(az), cz=cos(az);
        double r[3][3] = {
            { cy*cz,                cy*sz,                -sy    },
            { sx*sy*cz - cx*sz,     sx*sy*sz + cx*cz,      sx*cy },
            { cx*sy*cz + sx*sz,     cx*sy*sz - sx*cz,      cx*cy },
        };
        double mv[16] = {0};
        for (int i=0;i<3;i++) for (int j=0;j<3;j++) mv[i*4+j] = r[i][j]*scale;
        mv[3*4+0]=412.0; mv[3*4+1]=-88.0; mv[3*4+2]=3050.0; mv[3*4+3]=1.0;
        double pr[16] = {0};
        pr[0]=2.1; pr[5]=3.2; pr[10]=-1.0002; pr[11]=-1.0; pr[14]=-60.0; pr[15]=0.0;
        double mvp[16] = {0};
        for (int i=0;i<4;i++) for (int j=0;j<4;j++) { double s=0; for (int k=0;k<4;k++) s += mv[i*4+k]*pr[k*4+j]; mvp[i*4+j]=s; }
        double ov_d[4] = { 1200.0, -340.0, 75.0, 1.0 };
        double ref[4];
        for (int j=0;j<4;j++) { double s=0; for (int i=0;i<4;i++) s += ov_d[i]*mvp[i*4+j]; ref[j]=s; }

        float m_f[16]; for (int i=0;i<16;i++) m_f[i] = (float)mvp[i];
        float v_f[4];  for (int i=0;i<4;i++)  v_f[i] = (float)ov_d[i];
        float out[4];
        geom_vpar_matvec_f(m_f, v_f, out);
        for (int j = 0; j < 4; j++) {
            double err = fabs((double)out[j] - ref[j]);
            printf("  clip[%d]: vpar=%.4f double=%.4f err=%.5f\n", j, out[j], ref[j], err);
            CHECK(err < 1.0, "clip[%d] error too large: %.5f", j, err);
        }
    }

    /* VMUL4 -- element-wise, the perspective-divide shape */
    {
        float invw = 1.0f / 37.4f;
        float a[4] = { 1200.0f, -340.0f, 75.0f, 37.4f };
        float b[4] = { invw, invw, invw, 1.0f };
        float out[4];
        geom_vpar_vmul4_f(a, b, out);
        float ref[4]; for (int i = 0; i < 4; i++) ref[i] = a[i] * b[i];
        for (int i = 0; i < 4; i++) {
            float tol = fabsf(ref[i]) * 0.02f + 0.01f;   /* S17.10's own quantization, see Vpu4DFixed.v's header */
            CHECK(fabsf(out[i] - ref[i]) < tol, "vmul4[%d]: got %.5f want %.5f", i, out[i], ref[i]);
        }
        if (fails == 0) printf("  ok   vmul4: (%.4f,%.4f,%.4f,%.4f)\n", out[0], out[1], out[2], out[3]);
    }

    /* VFMA4 -- the viewport scale+translate shape, incl. a negative seed
     * (the exact scale-mismatch/signedness bug class found in
     * rtl/vpu/Vpu4DFixed.v -- re-verified here at the firmware API layer,
     * not just the raw RTL testbench). */
    {
        float a[4] = { 0.5f, -0.25f, 3.0f, 1.0f };
        float b[4] = { 133.0f, -120.0f, 2.0f, 1.0f };
        float c[4] = { 133.0f, 120.0f, -500.0f, 0.0f };
        float out[4];
        geom_vpar_vfma4_f(a, b, c, out);
        float ref[4]; for (int i = 0; i < 4; i++) ref[i] = a[i] * b[i] + c[i];
        for (int i = 0; i < 4; i++) {
            float tol = fabsf(ref[i]) * 0.005f + 0.05f;
            CHECK(fabsf(out[i] - ref[i]) < tol, "vfma4[%d]: got %.5f want %.5f", i, out[i], ref[i]);
        }
        if (fails == 0) printf("  ok   vfma4: (%.4f,%.4f,%.4f,%.4f)\n", out[0], out[1], out[2], out[3]);
        /* c[2] = -500 is the negative-seed regression case: 3.0*2.0-500.0 = -494 */
        CHECK(fabsf(out[2] - (-494.0f)) < 1.0f, "vfma4 negative seed: got %.4f want -494", out[2]);
    }

    /* light4 -- 4 light-direction dot products against one normal, via
     * MATVEC4's column-per-lane reuse (no dedicated lighting hardware). */
    {
        float normal[3] = { 0.0f, 1.0f, 0.0f };   /* straight up */
        float lights[4][3] = {
            { 0.0f, 1.0f, 0.0f },     /* straight down onto it: dot = 1.0 */
            { 0.0f, -1.0f, 0.0f },    /* opposite: dot = -1.0 */
            { 1.0f, 0.0f, 0.0f },     /* perpendicular: dot = 0.0 */
            { 0.0f, 0.70710678f, 0.70710678f },  /* 45 deg: dot = 0.7071 */
        };
        float out[4];
        geom_vpar_light4(normal, lights, out);
        float ref[4] = { 1.0f, -1.0f, 0.0f, 0.70710678f };
        for (int i = 0; i < 4; i++)
            CHECK(fabsf(out[i] - ref[i]) < 0.005f, "light4[%d]: got %.5f want %.5f", i, out[i], ref[i]);
        if (fails == 0) printf("  ok   light4: (%.4f,%.4f,%.4f,%.4f)\n", out[0], out[1], out[2], out[3]);
    }

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails != 0;
}
