/* Invalidate the geom core's data cache, so that what other masters wrote to
 * SDRAM is read afresh. VexRiscv: its flush instruction. The MIPS geom core
 * (rtl/mips/mips_geom.sv): a store to GEOM_DCACHE_FLUSH_ADDR, which the
 * cache takes as the command and which never reaches the bus; the next
 * cached access waits until every line is invalid. */
#ifndef GEOM_DCACHE_H
#define GEOM_DCACHE_H
#include <stdint.h>
#ifdef __mips__
#define GEOM_DCACHE_FLUSH_ADDR 0x2000C000u
#define geom_dcache_flush() \
    __asm__ volatile("sw $0, 0(%0)" :: "r"(GEOM_DCACHE_FLUSH_ADDR) : "memory")
#else
#define geom_dcache_flush() __asm__ volatile(".word 0x500F" ::: "memory")
#endif
#endif
