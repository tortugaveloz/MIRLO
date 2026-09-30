/* crt0 for the geometry core on MIPS (rtl/mips/mips_geom.sv): the reset
 * vector is ROM A's base (0x2000_0000). That core has no exceptions, so no
 * trap vector: set the stack to the top of the RAM, clear .bss, copy .data's
 * image out of ROM A, run main. */
    .set noreorder
    .section .text.start, "ax"
    .globl _start
_start:
    la      $sp, _fstack
    la      $8, _fbss
    la      $9, _ebss
1:  sltu    $10, $8, $9
    beq     $10, $0, 2f
    nop
    sw      $0, 0($8)
    b       1b
    addiu   $8, $8, 4
2:  la      $8, _fdata
    la      $9, _edata
    la      $11, _fdata_lma
3:  sltu    $10, $8, $9
    beq     $10, $0, 4f
    nop
    lw      $12, 0($11)
    addiu   $11, $11, 4
    sw      $12, 0($8)
    b       3b
    addiu   $8, $8, 4
4:  jal     main
    nop
5:  b       5b
    nop
