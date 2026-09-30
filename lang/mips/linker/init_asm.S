# Start of a C/C++ program on MIRLO's MIPS game CPU (linked by c-linker.ld at
# 0x4000_0000, where the Pocket -- or the boot ROM's serial boot -- put it;
# the boot ROM jumps here with the caches invalidated).
#   0x4000_0000  _start
#   0x4000_0180  the exception vector (Status.BEV 0: mips_core's EXC_BASE +
#                0x180): the program's trap_handler if it links one (weak;
#                lang/c/game/log.c), with the caller-saved registers kept
#                and an eret after it; else a halt
    .set    noreorder
    .set    noat
    .globl  _start
    .weak   trap_handler
    .text
_start:
    b       1f
    nop

    .org    0x180
    # The exception vector. Only a program with a trap_handler (weak) takes
    # it: the caller-saved registers (integer, HI/LO, FPU $f0-$f19, FCSR)
    # go on the stack, trap_handler() runs, and eret returns to EPC -- an
    # interrupt resumes where it hit (lang/c/game/log.c: the watchdog, the
    # profiler, the periodic hook); a fault's handler does not return.
    la      $k0, trap_handler
    beqz    $k0, 9f
    nop
    addiu   $sp, $sp, -208
    sw      $at, 16($sp)
    sw      $v0, 20($sp)
    sw      $v1, 24($sp)
    sw      $a0, 28($sp)
    sw      $a1, 32($sp)
    sw      $a2, 36($sp)
    sw      $a3, 40($sp)
    sw      $t0, 44($sp)
    sw      $t1, 48($sp)
    sw      $t2, 52($sp)
    sw      $t3, 56($sp)
    sw      $t4, 60($sp)
    sw      $t5, 64($sp)
    sw      $t6, 68($sp)
    sw      $t7, 72($sp)
    sw      $t8, 76($sp)
    sw      $t9, 80($sp)
    sw      $ra, 84($sp)
    mfhi    $t0
    mflo    $t1
    sw      $t0, 88($sp)
    sw      $t1, 92($sp)
    cfc1    $t0, $31
    sw      $t0, 96($sp)
    .irp    n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19
    swc1    $f\n, 100+4*\n($sp)
    .endr
    jalr    $k0
    nop
    .irp    n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19
    lwc1    $f\n, 100+4*\n($sp)
    .endr
    lw      $t0, 96($sp)
    ctc1    $t0, $31
    lw      $t0, 88($sp)
    lw      $t1, 92($sp)
    mthi    $t0
    mtlo    $t1
    lw      $at, 16($sp)
    lw      $v0, 20($sp)
    lw      $v1, 24($sp)
    lw      $a0, 28($sp)
    lw      $a1, 32($sp)
    lw      $a2, 36($sp)
    lw      $a3, 40($sp)
    lw      $t0, 44($sp)
    lw      $t1, 48($sp)
    lw      $t2, 52($sp)
    lw      $t3, 56($sp)
    lw      $t4, 60($sp)
    lw      $t5, 64($sp)
    lw      $t6, 68($sp)
    lw      $t7, 72($sp)
    lw      $t8, 76($sp)
    lw      $t9, 80($sp)
    lw      $ra, 84($sp)
    addiu   $sp, $sp, 208
    .set    mips3
    eret
    .set    mips2
9:  b       9b
    nop

1:  # Status: CU1 (the FPU), CU0, FR (32 independent FPU registers); BEV 0,
    # kernel mode, interrupts off
    lui     $t0, 0x3400
    mtc0    $t0, $12
    nop
    ctc1    $zero, $31              # FCSR: round to nearest, no traps
    la      $sp, _fstack
    # .bss to 0
    la      $a0, _fbss
    la      $a1, _ebss
2:  beq     $a0, $a1, 3f
    nop
    sw      $zero, 0($a0)
    b       2b
    addiu   $a0, $a0, 4
3:  jal     main
    nop
4:  b       4b
    nop
