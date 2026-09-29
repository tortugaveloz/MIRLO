// Init file for C/C++ programs linking using `c-linker.ld`

.global main
.global _start
.weak trap_handler

_start:
  // Make sure stack starts where we want
  la sp, _fstack

  // Initialize bss block to 0
  la a0, _fbss
  la a1, _ebss
bss_loop:
  beq a0,a1,bss_done
  sw zero,0(a0)
  add a0,a0,4
  j bss_loop
bss_done:
  // Install the program's own trap vector if it has one (weak: most programs
  // don't, and then the BIOS's stays). See lang/c/game/log.c.
  la t0, trap_handler
  beqz t0, 1f
  csrw mtvec, t0
1:
  call main
