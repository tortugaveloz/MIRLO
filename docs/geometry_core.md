# The geometry core

The geometry core is Mirlo's second MIPS core. It plays the role of the
N64's RSP:
* it walks the display list (GDL) the game CPU writes;
* it keeps the matrix stack, and transforms, lights, fogs and clips the
  vertices;
* it decodes textures;
* it sets up each triangle and sends MRDP commands (`docs/mrdp.md`).

## The core

* **CPU:** `mips_geom` (`rtl/mips/mips_geom.sv`): `mips_lite` (MIPS32's
  integer subset, with a divider and clz) plus a write-through data cache,
  with **no FPU**. `rtl/soc/n64_geom.sv` puts it together with its memories
  and its CFU.
* **ROM**, with no I-cache: the core fetches straight from it.
  * **ROM A** (`0x2000_0000`, 16 KiB) is also on the data bus.
  * **ROM B** (`0x2001_0000`, 64 KiB) is **fetch only**. `geom_mips.ld`'s
    `.text_b` may hold code only: a load from ROM B returns nothing useful.
    `sim/geom_full` warns about it.
* **RAM** at `0x2000_8000`, 16 KiB: the core's tightly coupled data memory.
  Its second port is on the SoC bus, so the game CPU can read the geometry
  core's variables (addresses in `lang/c/geom/build/geom.map`).
* **Firmware:** `lang/c/geom`, built `-O2 -fno-jump-tables`. A jump table is a
  data read from ROM and costs far more than a compare tree.

## The firmware is part of the bitstream

`lang/c/geom/build/geom.bin` is baked into the ROMs at synthesis
(`tools/mips_inits.py` writes `rtl/soc/init/geom_rom_{a,b}.hex`). It also has
the register addresses compiled in. So:

* A change under `lang/c/geom` needs a Quartus compile.
* A change to the register map (`tools/mirlo_regs.py`) needs geom.bin rebuilt
  against it.

The build order is:
1. `python3 tools/mirlo_regs.py`;
2. `make clean && make` in `lang/c/geom`;
3. `python3 tools/mips_inits.py`;
4. run Quartus.

A firmware-only change can skip the fit: rewrite the Quartus db's
`*ram0_n64_geom*.hdl.mif` / `*ram1_n64_geom*.hdl.mif` (descending addresses,
binary) from the new hex files, then `quartus_cdb --update_mif` and
`quartus_asm`. A plain `--update_mif` re-reads the db's copies, not the hex
files, and keeps the old image.

`lang/c/geom`'s Makefile does not track header dependencies: `make clean &&
make` after editing a header.

## Numbers: fixed point everywhere

* **Storage only:** the GDL carries IEEE `float` bit patterns for the
  interface's sake. On this core, `float` is only a storage format.
* **S15.16 for scalar work:** `lang/c/geom/geom_fixed.c`. This is the N64's
  own matrix format (libultra's `guMtxF2L` scales by 65536). Hot paths decode
  once, compute in `fx_*` and encode once.
* **S17.10 for the vector unit's transform.** 27-bit operands, so each
  multiply fits one Cyclone V DSP block (27×27; 32×32 costs three). The
  largest values measured on real game content: matrix elements ~17,424 and
  clip-space coordinates ~23,659. That leaves >2.7× headroom.
* **Output to MRDP:** TRI_R triangles with MRDP's fixed-point vertex fields
  (`lang/c/mrdp/mrdp_setup.h`).

Host builds use `-ffp-contract=off`, as the target does, so that the host
reference and the hardware round identically.

## The vector unit (CFU)

The core's only arithmetic hardware beyond its integer unit sits in its CFU
(custom function unit), reached with MIPS opcode `0x1F` (`lang/c/mips/mips_cfu.h`,
`lang/c/geom/geom_vpar.h`).
It is not on any bus; the game CPU cannot reach it.

* **`rtl/vpu/Vpu4DFixed.v`:** four multiply-accumulate lanes, one DSP block
  each, running at once. It provides:
  * `MATVEC4`: the 4×4 transform, 8 cycles;
  * `VMUL4`, `VFMA4`: element-wise, for the divide and the viewport map;
  * lighting: 4 dot products per `MATVEC4`.

  Its header holds the function-id map.
* **`rtl/vpu/GeomSetupUnit.v`:** scalar setup ops, bit-exact with the C they
  replace:
  * blends, `fx_mul`;
  * a normalised reciprocal (a 512-entry seed table + one Newton step, as
    the RSP's VRCP);
  * 1/sqrt (for normals) and projection;
  * `PUSH` / `APPEND2`: send words straight into MRDP's command FIFO. The
    push stalls while the FIFO is full, so nothing is dropped.

  Its header holds the function-id map. The firmware probes at boot which
  ops the unit was built with (`SU_NO_*` macros in the `.qsf`) and falls back
  to C for the rest.

**Area rule:** every `*` in Verilog is its own multiplier to Quartus.
Three multipliers with FSM-steered operands do the work of thirteen.

## Memory the geometry core uses in SDRAM

| range | what |
|---|---|
| `0x4100_0000`.. | GDL ring (`lang/c/game/frame.c`) |
| `0x4120_0000`.. | texture staging for LOAD TILE (`geom_mrdp.h`) |
| `0x4130_0000`.. | configuration word and per-frame counters (`geom_cfg.h`) |
| `0x4140_0000`..`0x4160_8000` | decoded-texture cache: pages + table (`geom_mrdp.h`) |

## Verification

| what | how |
|---|---|
| host tests | `make hosttest` in `lang/c/geom` |
| vector unit | `sim/vpu`: `make -f Makefile.fixed run`, `make -f Makefile.setup run` |
| whole frames on the RTL | `sim/geom_full/check_host_vs_rtl.sh`: the real geom.bin on the real mips_geom + CFU must produce a command stream word-identical to the host build. Run it before any geometry-core Quartus build. `BUS_WAIT=30` adds SoC-bus latency. `PCPROF=` gives a per-function cycle profile. |
