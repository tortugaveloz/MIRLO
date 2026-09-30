/* crt0 for the geometry core (standalone rv32im, ilp32).
 *
 * The geometry core's reset vector is the base of its private scratchpad
 * (GEOM_SRAM_BASE = 0x2000_0000, see litex/analogue_pocket.py). This runs
 * from there: set the stack to the top of the scratchpad, zero .bss, copy
 * .data (it is linked/loaded in-place in the scratchpad image, so the copy
 * is a no-op unless .data has a separate LMA), then call main().
 */
#ifndef GEOM_TRAP_MSG
#define GEOM_TRAP_MSG 0xf000480c            /* CSR_MAILBOX_GEOM_MSG_ADDR (the RISC-V SoC's) */
#endif
    .equ MAILBOX_GEOM_MSG, GEOM_TRAP_MSG

    .section .text.start, "ax"
    .global _start
_start:
    /* no interrupts until main() enables them */
    csrw    mie, zero
    csrw    mip, zero

    /* BRING-UP: trap vector -> trap_handler, so a fault (illegal instr, bus
     * error, ...) is visible on the geom->game mailbox instead of a silent
     * hang. */
    la      t0, trap_handler
    csrw    mtvec, t0

    la      sp, _fstack
    la      gp, __global_pointer$

    /* clear .bss */
    la      a0, _fbss
    la      a1, _ebss
1:  bgeu    a0, a1, 2f
    sw      zero, 0(a0)
    addi    a0, a0, 4
    j       1b
2:

    /* copy .data from LMA to VMA (usually identical -> loop exits immediately) */
    la      a0, _fdata
    la      a1, _edata
    la      a2, _fdata_lma
3:  bgeu    a0, a1, 4f
    lw      a3, 0(a2)
    sw      a3, 0(a0)
    addi    a0, a0, 4
    addi    a2, a2, 4
    j       3b
4:

    call    main

hang:
    wfi
    j       hang

/* --- BRING-UP trap handler ---------------------------------------------
 * Alternates writing 0xFA17_00<mcause> and 0xFA1E_<mepc[31:8]> to the
 * geom->game mailbox message CSR forever, so the game CPU polling
 * mailbox_geom_msg_read() sees "geom faulted" + why/where.
 */
    .p2align 2
    .global trap_handler
trap_handler:
    li      t1, MAILBOX_GEOM_MSG
    csrr    t2, mcause
    csrr    t3, mepc
1:
    li      t0, 0xFA170000
    or      t0, t0, t2
    sw      t0, 0(t1)
    li      t4, 0x40000
2:  addi    t4, t4, -1
    bnez    t4, 2b
    li      t0, 0xFA1E0000
    srli    t5, t3, 8
    or      t0, t0, t5
    sw      t0, 0(t1)
    li      t4, 0x40000
3:  addi    t4, t4, -1
    bnez    t4, 3b
    j       1b
