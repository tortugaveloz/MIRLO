/* picolibc on MIRLO's MIPS game CPU: printf, snprintf, malloc, the maths
 * library, soft doubles, 64-bit arithmetic. Prints PASS or the failures,
 * then 0x04 (sim/mips_soc stops). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <uart.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static volatile double d1 = 3.0, d2 = 7.0;
static volatile float f1 = 0.5f;
static volatile uint64_t q1 = 0x123456789ABCDEFull, q2 = 1000003;

int main(void)
{
    char b[64];
    printf("libc_test: printf %d %u %x %s\n", -42, 42u, 0xBEEFu, "ok");
    snprintf(b, sizeof b, "%05d|%-3s|%08lx", 17, "ab", 0xCAFEul);
    CHECK(strcmp(b, "00017|ab |0000cafe") == 0);
    char *m = malloc(1000); CHECK(m != NULL); memset(m, 0x5A, 1000); CHECK(m[999] == 0x5A); free(m);
    double d = d1 / d2; CHECK(d > 0.428571 && d < 0.428572);
    CHECK((int)(d * 1e6) == 428571);
    float s = sinf(f1), c = cosf(f1), r = sqrtf(2.0f);
    CHECK(fabsf(s - 0.4794255f) < 1e-6f); CHECK(fabsf(c - 0.8775826f) < 1e-6f); CHECK(fabsf(r - 1.4142135f) < 1e-6f);
    CHECK(fabsf(atan2f(1.0f, 1.0f) - 0.7853982f) < 1e-6f);
    CHECK(q1 / q2 == 81985529216486895ull / 1000003ull); CHECK(q1 % q2 == 81985529216486895ull % 1000003ull);
    CHECK((q1 >> 17) == 0x91A2B3C4D5ull);
    printf("libc_test: %s\n", fails ? "FAILED" : "PASS");
    uart_sync();
    uart_write(4);
    for (;;);
}
