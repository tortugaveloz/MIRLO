# MIRLO on MIPS, without LiteX

The `mips` branch moves MIRLO's three cores to MIPS and replaces the LiteX SoC
with a hand-written SystemVerilog one, built from the same fabric as MIRLO64
(the SDRAM controller, arbiter and scan-out). The point is to measure the
move: the geom core ran faster on MIPS in MIRLO64, and this branch shows what
the whole of Super Mirlo 64 does on it. It also brings MIRLO and MIRLO64
closer together.

| Core | RISC-V (LiteX, `master`) | MIPS (`mips`) |
|------|--------------------------|---------------|
| game CPU | VexRiscv-SMP, rv32imafc | `rtl/mips/mips_core.sv`: a 32-bit VR4300, little-endian, single-precision FPU, 16 KiB I / 8 KiB D cache |
| geom core | VexRiscvGeom, rv32i + Zmmul | `rtl/mips/mips_geom.sv` (mips_lite + D-cache), MIPS32 integer subset |
| audio core | VexRiscvAudio, rv32i + Zmmul | `rtl/mips/mips_lite.sv` |

The CFUs (`Vpu4DFixed` + the setup unit, `AudioDspCfu`), MRDP and the video
path are unchanged.

## The SoC (`rtl/soc/mirlo_mips.sv`)

`core_top.sv` builds it instead of the LiteX module when `MIRLO_MIPS` is
defined; the ports are the same. The memory map is MIRLO's, so the firmware
keeps its addresses:

| Address | What |
|---------|------|
| `0x2000_0000` | the geom core's ROM (its own); `0x2000_8000` its RAM, which the game CPU can also read and write |
| `0x4000_0000` | SDRAM, 64 MiB. The game CPU caches it; the program starts at its base |
| `0x40C0_0000` | the framebuffers (by convention) |
| `0x8000_0000` | the audio core: IMEM, DMEM, CTRL (the game loads it) |
| `0xF000_0000` | the registers: `rtl/soc/mirlo_regs.sv`, map in `tools/mirlo_regs.py` |
| `0xBFC0_0000` | the game CPU's boot ROM (`lang/mips/boot`) |

The game CPU has no TLB and no KSEG0: `mips_core` is built with `FLAT`, so
every address is physical except the boot ROM's KSEG1, SDRAM is the cached
span, and the exception vector is `0x4000_0180`. It is little-endian (`BIG`
0), like the other cores and like everything MIRLO's software writes to
memory.

`tools/mirlo_regs.py` generates the register decode (`rtl/soc/mirlo_regs.svh`)
and the firmware's `lang/mips/include/generated/{csr,soc,mem}.h`, with LiteX's
accessor names (`mrdp_status_read()`, `timer0_uptime_cycles_read()`, ...).

## Booting

The boot ROM does what MIRLO's patched LiteX BIOS does. It waits, touching
only registers, for the Pocket to release its reset after loading data slot 0
to `0x4000_0000`, then jumps there. If the reset was already released (a
bitstream loaded over JTAG) or nothing was loaded, it falls back to LiteX's
SFL serial boot over the JTAG UART, so `litex/litex_term.py` and
`litex/jtag_run.py` work as before.

## Software

- `lang/mips/mips.mk`: the toolchain (any MIPS GCC plus binutils; the flags
  set the byte order and ABI). The game CPU uses `-march=vr4300 -msingle-float`
  under o32; the helper cores use `-march=mips32 -msoft-float`.
- `lang/mips/lib.mk`: picolibc (LiteX's options), compiler-rt's builtins and
  `lib/mirlo_rt.c` (the UART, stdio, delays), built per variant:
  `make -f lib.mk VARIANT=game` and `VARIANT=lite`.
- `lang/mips/program.mk`, `lang/mips/linker/`: a game-CPU program (crt0 with
  an exception vector that saves registers, so timer interrupts work).
- `lang/c/geom`: `make CPU=mips` gives `build/mips/geom.bin`.
- `lang/c/game`: `make CPU=mips [PROG=...]` gives `build/mips/build.bin`.
- `tools/mips_inits.py`: the boot ROM and geom ROM images for the bitstream.

## Simulation

`sim/mips_soc/build.sh soc` verilates the whole SoC against an SDRAM chip
model. `obj_soc/tb_soc prog.bin -c <Mcycles> -o <dir>` puts the program where
the Pocket would load it, prints the UART, and dumps frames.
`lang/mips/test` holds a CPU self-test and a libc test; the cube demo is
`lang/c/game` with `CPU=mips`.
