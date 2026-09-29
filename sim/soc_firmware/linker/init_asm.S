// Init for the ROM-resident SoC test firmware (see linker/rom-linker.ld).
//
// Differs from ../../lang/linker/init_asm.S by copying .data out of ROM: on
// the Pocket the whole image is loaded into RAM so .data is already in place,
// but here .text/.rodata execute from ROM while .data lives in SRAM, so it has
// to be copied at startup or initialised globals contain garbage.

.global main
.global _start

_start:
  // Make sure stack starts where we want
  la sp, _fstack

  // Copy .data from its load address in ROM to its run address in SRAM
  la a0, _fdata          // dest
  la a1, _fdata_rom      // src
  la a2, _edata          // dest end
data_loop:
  beq a0,a2,data_done
  lw a3,0(a1)
  sw a3,0(a0)
  add a0,a0,4
  add a1,a1,4
  j data_loop
data_done:

  // Initialize bss block to 0
  la a0, _fbss
  la a1, _ebss
bss_loop:
  beq a0,a1,bss_done
  sw zero,0(a0)
  add a0,a0,4
  j bss_loop
bss_done:
  call main
