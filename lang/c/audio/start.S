/* Audio core entry. Reset vector = IMEM base (0x8000_0000); the trap vector
 * is fixed at IMEM + 0x20 (GenCoreAudio.scala: mtvecInit). */
    .section .text.start, "ax"
    .globl _start
_start:
    j       reset

    .balign 32
trap:
    /* Report and stop: MBOX5..7 = mcause/mepc/mtval, then MBOX0 = TRAP. */
    li      t0, 0x80004000
    csrr    t1, mcause
    sw      t1, 0x1C(t0)
    csrr    t1, mepc
    sw      t1, 0x20(t0)
    csrr    t1, mtval
    sw      t1, 0x24(t0)
    li      t1, 0xA0D1DEAD
    sw      t1, 0x08(t0)
1:  j       1b

reset:
    la      sp, _stack_top
    la      t0, _bss_start
    la      t1, _bss_end
2:  bgeu    t0, t1, 3f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       2b
3:  call    main
4:  j       4b
