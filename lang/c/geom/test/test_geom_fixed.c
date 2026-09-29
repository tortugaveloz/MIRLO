#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "geom_fixed.h"

static uint32_t f2u(float f){ union{float f; uint32_t u;} x; x.f=f; return x.u; }
static float u2f(uint32_t u){ union{float f; uint32_t u;} x; x.u=u; return x.f; }

static int fails = 0;
#define CHECK(cond, ...) do{ if(!(cond)){ printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } }while(0)

int main(void) {
    srand(12345);

    /* 1. bit round-trip: float -> fixed -> float within one ULP of the format step */
    float step = 1.0f / (float)FX_ONE;
    int nfail_rt = 0;
    for (int i = 0; i < 200000; i++) {
        float v = ((float)(rand() % 2000001) - 1000000.0f) / 100.0f; /* -10000..10000, 2 decimals */
        int32_t fx = fx_from_bits(f2u(v));
        float back = u2f(fx_to_bits(fx));
        float err = fabsf(back - v);
        if (err > step * 1.01f) { if (nfail_rt < 5) printf("roundtrip v=%g back=%g err=%g step=%g\n", v, back, err, step); nfail_rt++; }
    }
    CHECK(nfail_rt == 0, "round-trip: %d/200000 exceeded 1 step tolerance", nfail_rt);

    /* 2. exact 1/1000 case (a projection scale S17.10 loses 2.3 % on) */
    {
        int32_t fx = fx_from_bits(f2u(0.001f));
        float back = u2f(fx_to_bits(fx));
        float relerr = fabsf(back - 0.001f) / 0.001f;
        printf("0.001 -> fixed raw=%d -> back=%.8f relerr=%.6f%%\n", fx, back, relerr*100.0);
        CHECK(relerr < 0.01, "0.001 relative error too high: %.6f%%", relerr*100.0);
    }

    /* 3. fx_mul vs float multiply, structural range */
    int nfail_mul = 0;
    for (int i = 0; i < 200000; i++) {
        float a = ((float)(rand() % 2000001) - 1000000.0f) / 1000.0f; /* -1000..1000 */
        float b = ((float)(rand() % 200001) - 100000.0f) / 100000.0f; /* -1..1 */
        int32_t fa = fx_from_bits(f2u(a)), fb = fx_from_bits(f2u(b));
        int32_t fp = fx_mul(fa, fb);
        float got = u2f(fx_to_bits(fp));
        float ref = a * b;
        float tol = fabsf(a)*step*0.6f + fabsf(b)*step*0.6f + fabsf(ref)*2e-6f + step;
        if (fabsf(got - ref) > tol) { if (nfail_mul < 5) printf("mul a=%g b=%g got=%g ref=%g tol=%g\n", a,b,got,ref,tol); nfail_mul++; }
    }
    CHECK(nfail_mul == 0, "fx_mul: %d/200000 exceeded tolerance", nfail_mul);

    /* 4. fx_add / fx_sub */
    int nfail_add = 0;
    for (int i = 0; i < 200000; i++) {
        float a = ((float)(rand() % 2000001) - 1000000.0f) / 100.0f;
        float b = ((float)(rand() % 2000001) - 1000000.0f) / 100.0f;
        int32_t fa = fx_from_bits(f2u(a)), fb = fx_from_bits(f2u(b));
        float got_add = u2f(fx_to_bits(fx_add(fa, fb)));
        float got_sub = u2f(fx_to_bits(fx_sub(fa, fb)));
        float ref_add = a + b, ref_sub = a - b;
        float tol_add = step*2.0f + fabsf(ref_add)*2e-6f;
        float tol_sub = step*2.0f + fabsf(ref_sub)*2e-6f;
        if (fabsf(got_add - ref_add) > tol_add) { if(nfail_add<5) printf("add a=%g b=%g got=%g ref=%g tol=%g\n",a,b,got_add,ref_add,tol_add); nfail_add++; }
        if (fabsf(got_sub - ref_sub) > tol_sub) { if(nfail_add<5) printf("sub a=%g b=%g got=%g ref=%g tol=%g\n",a,b,got_sub,ref_sub,tol_sub); nfail_add++; }
    }
    CHECK(nfail_add == 0, "fx_add/sub: %d exceeded tolerance", nfail_add);

    /* 5. fx_recip */
    int nfail_recip = 0;
    for (int i = 0; i < 100000; i++) {
        float a = ((float)(rand() % 2000000) - 1000000.0f) / 100.0f;
        if (fabsf(a) < 0.01f) continue;
        int32_t fa = fx_from_bits(f2u(a));
        float got = u2f(fx_to_bits(fx_recip(fa)));
        float ref = 1.0f / a;
        float tol = fabsf(ref) * 0.002f + step * 4.0f;
        if (fabsf(got - ref) > tol) { if(nfail_recip<5) printf("recip a=%g got=%g ref=%g\n", a, got, ref); nfail_recip++; }
    }
    CHECK(nfail_recip == 0, "fx_recip: %d exceeded tolerance", nfail_recip);

    /* 6. fx_wsum3 (barycentric blend structure: weights sum ~1, in [0,1]) */
    int nfail_wsum = 0;
    for (int i = 0; i < 100000; i++) {
        float a = ((float)(rand()%20001)-10000.0f)/10.0f;
        float b = ((float)(rand()%20001)-10000.0f)/10.0f;
        float c = ((float)(rand()%20001)-10000.0f)/10.0f;
        float w0 = (float)(rand()%1001)/1000.0f;
        float w1 = (float)(rand()%1001)/1000.0f * (1.0f - w0);
        float w2 = 1.0f - w0 - w1;
        int32_t fa=fx_from_bits(f2u(a)), fb=fx_from_bits(f2u(b)), fc=fx_from_bits(f2u(c));
        int32_t fw0=fx_from_bits(f2u(w0)), fw1=fx_from_bits(f2u(w1)), fw2=fx_from_bits(f2u(w2));
        float got = u2f(fx_to_bits(fx_wsum3(fa,fw0,fb,fw1,fc,fw2)));
        float ref = a*w0 + b*w1 + c*w2;
        float tol = fabsf(ref)*0.002f + step*8.0f + 0.01f;
        if (fabsf(got-ref) > tol) { if(nfail_wsum<5) printf("wsum3 a=%g b=%g c=%g w=(%g,%g,%g) got=%g ref=%g\n",a,b,c,w0,w1,w2,got,ref); nfail_wsum++; }
    }
    CHECK(nfail_wsum == 0, "fx_wsum3: %d exceeded tolerance", nfail_wsum);

    /* 6b. fx_div_ii(num, den) -- RAW ints, den can be millions in magnitude
     * (the triangle-area case that originally motivated this function, and
     * later its own reciprocal+multiply rewrite -- never had a dedicated
     * test until this one). Covers both small-magnitude and the
     * multi-million-magnitude regime that broke the old vpu_itof()-based
     * approach. */
    int nfail_div = 0;
    for (int i = 0; i < 200000; i++) {
        int32_t num = (rand() % 200001) - 100000;               /* -100000..100000 */
        int32_t den = (rand() % 6000001) - 3000000;              /* -3e6..3e6 */
        if (den == 0) continue;
        int32_t fx = fx_div_ii(num, den);
        float got = u2f(fx_to_bits(fx));
        float ref = (float)num / (float)den;
        float tol = fabsf(ref) * 0.002f + step * 4.0f + 1e-6f;
        if (fabsf(got - ref) > tol) { if (nfail_div < 5) printf("div_ii num=%d den=%d got=%g ref=%g tol=%g\n", num, den, got, ref, tol); nfail_div++; }
    }
    CHECK(nfail_div == 0, "fx_div_ii: %d/200000 exceeded tolerance", nfail_div);
    {
        /* the exact bug scenario: wInit[k]/area with area in the millions */
        int32_t fx = fx_div_ii(97331, 2833691);
        float got = u2f(fx_to_bits(fx));
        float ref = 97331.0f / 2833691.0f;
        CHECK(fabsf(got - ref) < 0.001f, "fx_div_ii regression case: got=%.6f ref=%.6f", got, ref);
    }
    CHECK(fx_div_ii(5, 0) == FX_MAX_RAW, "fx_div_ii den=0, num>=0 -> FX_MAX_RAW");
    CHECK(fx_div_ii(-5, 0) == FX_MIN_RAW, "fx_div_ii den=0, num<0 -> FX_MIN_RAW");

    /* 7. edge cases */
    CHECK(fx_from_bits(0) == 0, "zero bits -> 0");
    CHECK(fx_to_bits(0) == 0, "zero fixed -> zero bits");
    CHECK(fx_from_int(5) == 5*FX_ONE, "fx_from_int(5)");
    CHECK(fx_to_int_round(fx_from_int(5)) == 5, "int roundtrip");
    CHECK(fx_to_int_round(fx_from_bits(f2u(4.6f))) == 5, "round 4.6 -> 5");
    CHECK(fx_to_int_round(fx_from_bits(f2u(-4.6f))) == -5, "round -4.6 -> -5");

    if (fails == 0) printf("\nALL PASS (S15.16 fixed-point primitives, pure integer, no FPU)\n");
    else printf("\n%d CHECK(S) FAILED\n", fails);
    return fails ? 1 : 0;
}
