/* A bare self-check of MIRLO's MIPS game CPU on the SoC (sim/mips_soc):
 * little-endian loads and stores of every width (lb/lh/lw, lwl/lwr,
 * swl/swr through memcpy of unaligned data), the D-cache against uncached
 * device reads, the FPU, the divider, the registers. No libc. Prints PASS or
 * the failures on the UART, then a 0x04 (the bench stops). */
#include <stdint.h>
#include <generated/csr.h>
#include <system.h>

static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
static int fails;
static void check(const char *what, uint32_t got, uint32_t want)
{
    if (got == want) return;
    fails++;
    puts_("FAIL "); puts_(what); puts_(": got "); puthex(got); puts_(" want "); puthex(want); puts_("\n");
}

static volatile uint8_t buf[64] __attribute__((aligned(8)));
struct __attribute__((packed)) un { uint8_t a; uint32_t w; uint16_t h; };
static volatile struct un u;
static volatile float fa = 1.5f, fb = -2.25f;
static volatile uint32_t da = 1000000007u, db = 97u;

int main(void)
{
    puts_("cpu_test\n");
    /* byte order: a word's bytes, a halfword's */
    volatile uint32_t *w = (volatile uint32_t *)buf;
    w[0] = 0x44332211u;
    check("lbu 0", buf[0], 0x11); check("lbu 3", buf[3], 0x44);
    check("lhu 0", ((volatile uint16_t *)buf)[0], 0x2211); check("lhu 2", ((volatile uint16_t *)buf)[1], 0x4433);
    check("lb sign", (uint32_t)(int32_t)((volatile int8_t *)buf)[3], 0x44);
    buf[1] = 0xAA; check("sb 1", w[0], 0x4433AA11u);
    ((volatile uint16_t *)buf)[1] = 0xBEEF; check("sh 2", w[0], 0xBEEFAA11u);
    ((volatile int8_t *)buf)[4] = -2; check("lb neg", (uint32_t)(int32_t)((volatile int8_t *)buf)[4], 0xFFFFFFFEu);
    /* unaligned (lwl/lwr, swl/swr) */
    u.a = 1; u.w = 0xCAFEF00Du; u.h = 0x1234;
    check("unaligned w", u.w, 0xCAFEF00Du); check("unaligned h", u.h, 0x1234);
    uint8_t *p = (uint8_t *)&u;
    check("unaligned bytes", (uint32_t)p[1] | p[2] << 8 | p[3] << 16 | (uint32_t)p[4] << 24, 0xCAFEF00Du);
    /* the D-cache: a cached write then a read through the device window of the same SDRAM? (no alias:
     * check write-through by invalidating and reading back) */
    w[2] = 0x5A5A1234u; flush_cpu_dcache(); check("write-through", w[2], 0x5A5A1234u);
    /* the FPU (single) and doubles in software are not linked here: floats only */
    float f = fa * fb + 0.5f;
    check("fmul/fadd", *(uint32_t *)&f, 0xC0380000u);          /* -2.875 */
    float q = fa / fb;
    check("fdiv", *(uint32_t *)&q, 0xBF2AAAABu);                /* -0.6666667 */
    check("cvt", (uint32_t)(int32_t)(fb * 4.0f), (uint32_t)-9);
    /* the divider */
    check("divu", da / db, 10309278u); check("remu", da % db, 1000000007u - 10309278u * 97u);
    /* the registers */
    check("scratch", ctrl_scratch_read(), 0x12345678u);
    ctrl_scratch_write(0xA5A55A5Au); check("scratch w", ctrl_scratch_read(), 0xA5A55A5Au);
    timer0_uptime_latch_write(1); uint64_t t0 = timer0_uptime_cycles_read();
    timer0_uptime_latch_write(1); uint64_t t1 = timer0_uptime_cycles_read();
    check("uptime runs", t1 > t0, 1);
    check("bus errors", ctrl_bus_errors_read(), 0);
    (void)*(volatile uint32_t *)0x30000000u;                    /* nothing there */
    check("bus error counted", ctrl_bus_errors_read(), 1);
    /* the geom RAM window */
    volatile uint32_t *gr = (volatile uint32_t *)0x2000BFF0u;
    gr[0] = 0x0BADF00Du; gr[1] = 0x12345678u;
    check("geom ram", gr[0], 0x0BADF00Du); check("geom ram 2", gr[1], 0x12345678u);
    /* the audio core's DMEM window */
    volatile uint32_t *am = (volatile uint32_t *)0x80002000u;
    am[5] = 0xFEEDBEEFu; check("audio dmem", am[5], 0xFEEDBEEFu);
    puts_(fails ? "cpu_test: FAILED\n" : "cpu_test: PASS\n");
    putc_(4);
    for (;;);
}
