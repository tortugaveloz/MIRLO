/* The CFU instruction on the helper cores' MIPS (rtl/mips/mips_lite.sv):
 *
 *     [31:26] = 0x1F  rs  rt  rd  fid[10:0]        rd = cfu(fid[9:0], rs, rt)
 *
 * GAS has no generic encoder for it, so `mcfu fid, rd, rs, rt` is a macro:
 * it turns the register names GCC prints ($0..$31, and $sp, $fp and the
 * other named ones) into their numbers -- an unknown one is an assembly
 * error, never a silently wrong field -- and
 * assembles the word, and the compiler allocates the registers as for any
 * instruction. MCFU(id, a, b) is the C form. */
#ifndef MIPS_CFU_H
#define MIPS_CFU_H
#include <stdint.h>

__asm__(".ifndef _MCFU_DEFINED\n.set _MCFU_DEFINED, 1\n"
        ".macro _mcfu_reg sym, reg\n"
        ".set \\sym, -1\n"
        ".irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n"
        ".ifc \"\\reg\",\"$\\n\"\n.set \\sym, \\n\n.endif\n"
        ".endr\n"
        /* the registers GCC prints by name */
        ".ifc \"\\reg\",\"$zero\"\n.set \\sym, 0\n.endif\n"
        ".ifc \"\\reg\",\"$at\"\n.set \\sym, 1\n.endif\n"
        ".ifc \"\\reg\",\"$gp\"\n.set \\sym, 28\n.endif\n"
        ".ifc \"\\reg\",\"$sp\"\n.set \\sym, 29\n.endif\n"
        ".ifc \"\\reg\",\"$fp\"\n.set \\sym, 30\n.endif\n"
        ".ifc \"\\reg\",\"$ra\"\n.set \\sym, 31\n.endif\n"
        ".if \\sym < 0\n.error \"mcfu: unknown register \\reg\"\n.endif\n"
        ".endm\n"
        ".macro mcfu fid, rd, rs, rt\n"
        "_mcfu_reg _mcfu_d, \\rd\n_mcfu_reg _mcfu_s, \\rs\n_mcfu_reg _mcfu_t, \\rt\n"
        ".word 0x7C000000 | (_mcfu_s << 21) | (_mcfu_t << 16) | (_mcfu_d << 11) | ((\\fid) & 0x3FF)\n"
        ".endm\n.endif\n");

#define MCFU(id, a, b) ({ uint32_t _mcfu_r; \
    __asm__ volatile("mcfu %3, %0, %1, %2" : "=r"(_mcfu_r) \
                     : "r"((uint32_t)(a)), "r"((uint32_t)(b)), "i"(id)); _mcfu_r; })

#endif
