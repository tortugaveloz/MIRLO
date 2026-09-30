/* The game CPU's trap and timer interrupt, per architecture (log.c's trap
 * reporter, stall watchdog, PC profiler and periodic hook):
 *   RISC-V (the LiteX SoC): mcause/mepc/mtval, the CLINT's machine timer;
 *   MIPS (rtl/soc/mirlo_mips.sv): COP0 Cause/EPC/BadVAddr, Count/Compare
 *     (IP7; Count runs at half the clock; time from TIMER0's uptime).
 *     trap_handler() is called by
 *     lang/mips/linker/init_asm.S's vector with the caller-saved registers
 *     saved, and eret follows its return. */
#ifndef TRAP_ARCH_H
#define TRAP_ARCH_H
#include <stdint.h>

#ifdef __mips__
#include <generated/csr.h>
#define TRAP_HANDLER_ATTR
static inline uint32_t trap_cause(void) { uint32_t v; __asm__ volatile("mfc0 %0, $13" : "=r"(v)); return v; }
static inline uint32_t trap_epc(void)   { uint32_t v; __asm__ volatile("mfc0 %0, $14" : "=r"(v)); return v; }
static inline uint32_t trap_tval(void)  { uint32_t v; __asm__ volatile("mfc0 %0, $8"  : "=r"(v)); return v; }
static inline int trap_is_irq(uint32_t c)   { return ((c >> 2) & 31u) == 0u; }
static inline int trap_is_timer(uint32_t c) { return trap_is_irq(c) && (c & (1u << 15)); }
static inline uint32_t mips_status(void) { uint32_t v; __asm__ volatile("mfc0 %0, $12" : "=r"(v)); return v; }
static inline void mips_set_status(uint32_t v) { __asm__ volatile("mtc0 %0, $12\n\tnop" :: "r"(v)); }
/* the timer interrupt `cycles` sys cycles from now (a Compare write also clears it) */
static inline void timer_irq_arm(unsigned long cycles)
{
    uint32_t c; __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    __asm__ volatile("mtc0 %0, $11" :: "r"(c + (uint32_t)(cycles >> 1)));
}
static inline void timer_irq_enable(void)  { mips_set_status(mips_status() | (1u << 15) | 1u); }
static inline void timer_irq_disable(void) { mips_set_status(mips_status() & ~(1u << 15)); }
/* an interrupt nobody handles: mask it where it comes from */
static inline void trap_mask_stray(uint32_t c) { mips_set_status(mips_status() & ~(c & 0xFF00u)); }
/* sys-clock time (64-bit: TIMER0's uptime), and the timer interrupt at time t */
static inline uint64_t timer_now(void) { timer0_uptime_latch_write(1); return timer0_uptime_cycles_read(); }
static inline void timer_irq_at(uint64_t t)
{
    uint64_t now = timer_now();
    timer_irq_arm(t > now + 2u ? (unsigned long)(t - now) : 2ul);
}
/* interrupts off / back as they were (Status.IE) */
static inline unsigned long irq_save(void)
{
    uint32_t s = mips_status();
    mips_set_status(s & ~1u);
    return s & 1u;
}
static inline void irq_restore(unsigned long m) { if (m) mips_set_status(mips_status() | 1u); }
#else
#include <generated/mem.h>
#define TRAP_HANDLER_ATTR __attribute__((interrupt("machine"), aligned(4)))
static inline uint32_t trap_cause(void) { uint32_t v; __asm__ volatile("csrr %0, mcause" : "=r"(v)); return v; }
static inline uint32_t trap_epc(void)   { uint32_t v; __asm__ volatile("csrr %0, mepc"   : "=r"(v)); return v; }
static inline uint32_t trap_tval(void)  { uint32_t v; __asm__ volatile("csrr %0, mtval"  : "=r"(v)); return v; }
static inline int trap_is_irq(uint32_t c)   { return (c & 0x80000000u) != 0; }
static inline int trap_is_timer(uint32_t c) { return c == 0x80000007u; }
#define CLINT_REG(off) (*(volatile uint32_t *)(uintptr_t)(CLINT_BASE + (off)))
static inline uint64_t clint_mtime(void)
{
    uint32_t hi, lo;
    do { hi = CLINT_REG(0xBFFC); lo = CLINT_REG(0xBFF8); } while (hi != CLINT_REG(0xBFFC));
    return ((uint64_t)hi << 32) | lo;
}
static inline void timer_irq_arm(unsigned long cycles)
{
    uint64_t t = clint_mtime() + cycles;
    CLINT_REG(0x4004) = 0xFFFFFFFFu;        /* no spurious match mid-update */
    CLINT_REG(0x4000) = (uint32_t)t;
    CLINT_REG(0x4004) = (uint32_t)(t >> 32);
}
static inline void timer_irq_enable(void)
{
    __asm__ volatile("csrs mie, %0" :: "r"(1u << 7));       /* MTIE */
    __asm__ volatile("csrs mstatus, %0" :: "r"(1u << 3));   /* MIE */
}
static inline void timer_irq_disable(void) { __asm__ volatile("csrc mie, %0" :: "r"(1u << 7)); }
static inline void trap_mask_stray(uint32_t c) { (void)c; __asm__ volatile("csrc mstatus, %0" :: "r"(1u << 7)); }  /* MPIE: stay masked */
static inline uint64_t timer_now(void) { return clint_mtime(); }
static inline void timer_irq_at(uint64_t t)
{
    CLINT_REG(0x4004) = 0xFFFFFFFFu;
    CLINT_REG(0x4000) = (uint32_t)t;
    CLINT_REG(0x4004) = (uint32_t)(t >> 32);
}
static inline unsigned long irq_save(void)
{
    unsigned long m;
    __asm__ volatile("csrrci %0, mstatus, 8" : "=r"(m));
    return m & 8u;
}
static inline void irq_restore(unsigned long m) { if (m) __asm__ volatile("csrsi mstatus, 8"); }
#endif
#endif
