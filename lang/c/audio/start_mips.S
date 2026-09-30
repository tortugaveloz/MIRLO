/* Audio core entry on MIPS (rtl/mips/mips_lite.sv): the reset vector is the
 * IMEM base (0x8000_0000). No exceptions exist on that core, so no trap
 * vector: set the stack, clear .bss, run main. */
    .set noreorder
    .section .text.start, "ax"
    .globl _start
_start:
    la      $sp, _stack_top
    la      $8, _bss_start
    la      $9, _bss_end
1:  sltu    $10, $8, $9
    beq     $10, $0, 2f
    nop
    sw      $0, 0($8)
    b       1b
    addiu   $8, $8, 4
2:  jal     main
    nop
3:  b       3b
    nop
