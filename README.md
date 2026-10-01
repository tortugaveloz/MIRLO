# Mirlo

Mirlo is a MIPS computer for the [Analogue Pocket](https://www.analogue.co/pocket), built on the openFPGA platform, with hardware for 3D graphics in the style of the Nintendo 64. It is a system-on-chip for the Pocket's Cyclone V FPGA, written in SystemVerilog, plus the SDK to write software for it in C.

> **Experimental branch (`mips`).** Up to v0.8.2 Mirlo was a LiteX SoC with three RISC-V cores (branch `main`). This branch moves all three cores to MIPS and drops LiteX; see [docs/mips.md](docs/mips.md). Games built for the RISC-V Mirlo do not run here: rebuild them for MIPS.

Mirlo started as a fork of [openfpga-litex](https://github.com/agg23/openfpga-litex) by agg23.

## What is inside

Three MIPS cores, a rasterizer and the Pocket's peripherals:

* **Game CPU**: a 32-bit VR4300-class MIPS (`rtl/mips/mips_core.sv`), little-endian, with a single-precision FPU, 16 KiB I-cache and 8 KiB D-cache, running from the Pocket's 64 MiB SDRAM. Your program runs here.
* **Geometry core**: a second MIPS (`rtl/mips/mips_geom.sv`, MIPS32's integer subset) with a fixed-point vector unit as its CFU (custom function unit). It walks the display lists the game CPU writes: it transforms, lights and clips vertices, and sets up triangles. See [docs/geometry_core.md](docs/geometry_core.md).
* **MRDP**: an N64-style rasterizer. It takes the RDP's command format and provides:
  * TMEM and tiles, with a 3-point filter;
  * the N64 colour combiner and blender;
  * a 16-bit Z buffer;
  * RGB565 framebuffers.

  See [docs/mrdp.md](docs/mrdp.md).
* **Audio core**: a third MIPS (`rtl/mips/mips_lite.sv`) with a DSP CFU for mixing and ADPCM, feeding the Pocket's 48 kHz audio. Its programs are loaded by the game CPU at run time (`lang/c/audio`).
* **The Pocket's peripherals**:
  * input: all four controllers, including analog sticks and triggers;
  * video: 268 × 240 (fills the Pocket's screen) or 320 × 240;
  * vblank and a frame counter;
  * audio;
  * file access through the APF bridge (data slots, saves);
  * core settings through `interact.json`;
  * a JTAG UART for logs and program upload.

The register reference is [docs/control.md](docs/control.md); the map itself is `tools/mirlo_regs.py`, which generates both the hardware's decode and the firmware's `csr.h`.

## Installing on the Pocket

1. Download `tortuga.Mirlo_<version>.zip` from the [releases](https://github.com/tortugaveloz/MIRLO/releases).
2. Unzip it onto the **root** of the Pocket's SD card, merging with the folders already there. It adds only Mirlo's own files:

   ```
   Assets/mirlo/common/          where Mirlo's games go (empty)
   Cores/tortuga.Mirlo/          the core: mirlo.rev (the bitstream), core.json, ...
   Platforms/mirlo.json          the platform, under Computer
   Platforms/_images/mirlo.bin   its image
   ```

3. Put the games (`.bin` files) in `Assets/mirlo/common/`. Mirlo needs one to start: the release carries none; build one with the SDK (see [Writing software](#writing-software)) or get a Mirlo game such as [Super Mirlo 64](https://github.com/tortugaveloz/supermirlo64).
4. On the Pocket: **openFPGA → Computer → Mirlo**, then pick a game.

Saves (`.sav`) appear in `Saves/mirlo/common/` when you quit a game. To update the core, unzip a newer release the same way; it replaces the files in `Cores/tortuga.Mirlo/`.

## Using the core

### Games

When you launch Mirlo, the Pocket asks for a game: any `.bin` in `Assets/mirlo/common/`.
* The Pocket itself loads the file into SDRAM, with its own loading bar.
* The boot ROM then jumps to the program at `0x4000_0000`.

To restart a program, quit the core and launch it again.

A game file is a plain program image, or one packed with `tools/make_mirlo_game.py`. A packed file adds:
* data blocks the program reads itself;
* a footer with the video mode (268 or 320 wide).

### Over JTAG

With a USB Blaster on the Pocket's JTAG port you can load a bitstream and a program without touching the SD card:

```bash
tools/jtag/jtag_program_and_run.sh <bitstream.sof> <program.bin> <log> [seconds]
```

This programs the FPGA with `quartus_pgm`, then uploads the program through the boot ROM's serial boot (LiteX's SFL protocol), over the JTAG UART, and logs its output. It needs:
* `openocd`;
* Quartus' `quartus_pgm` (set `QUARTUS_PGM` if it is not in `~/altera/25.1std`).

Launch the core from the Pocket's menu once first, so that its bridge is up. The upload runs at about 3 KB/s, so it suits small programs; large games belong on the SD card. `tools/jtag/jtag_uart_relay.py` alone gives you the UART on a PTY.

Use a genuine USB Blaster: clones have damaged Pockets.

### Core settings

`interact.json` offers:
* **JTAG UART**: route the UART over JTAG.
* **Show FPS**, **Start = Select+Start**, **R = modifier**, **Stick**, **D-pad** and **L** (the N64 L button from a Dock controller's L2, R2, L3 or R3): settings a program can read from the `APF_INTERACT` registers (see [docs/control.md](docs/control.md#interact-api)). The SDK does not act on them itself; edit `interact.json` to offer your own.

## Writing software

Guides and examples: [lang/README.md](lang/README.md).

* **C**: `lang/c/examples/*`. `lang/c/game` is the 3D SDK: frame loop, display lists and the geometry-core hand-off.
* `lang/mips` is the SDK's base: the toolchain flags (`mips.mk`), picolibc and compiler-rt built for the cores (`lib.mk`), the C runtime and linker files, and `program.mk` for a game-CPU program.

The game CPU's code is `-march=vr4300 -mabi=32 -EL -msingle-float` (no 64-bit instructions; doubles in software). Any MIPS GCC with its binutils works; `lang/mips/mips.mk` names them.

Register addresses can change between hardware revisions: take them from the generated `csr.h`, never from memory.

## Building the hardware

### Tools

* **A MIPS GCC and binutils** (e.g. Ubuntu's `gcc-mips-linux-gnu`; the flags choose the byte order and ABI). Set `MIPS_CC` and `MIPS_BIN` for `lang/mips/mips.mk`.
* **Python 3**, with **meson** and **ninja** to build picolibc.
* **Quartus Prime Lite** with Cyclone V support (tested with 25.1std).
* **Verilator** for the simulations under `sim/`.

### Steps

```bash
git clone --recursive -b mips https://github.com/tortugaveloz/MIRLO.git
cd MIRLO

python3 tools/mirlo_regs.py                        # the register map: rtl/soc/mirlo_regs.svh, csr.h
(cd lang/mips && make -f lib.mk VARIANT=game && make -f lib.mk VARIANT=lite)   # picolibc, compiler-rt
(cd lang/mips/boot && make)                        # the game CPU's boot ROM
(cd lang/c/geom && make)                           # the geometry core's firmware
python3 tools/mips_inits.py                        # both images, for the bitstream

cd projects
quartus_sh --flow compile mirlo
python3 ../tools/package_bitstream.py output_files/mirlo.rbf mirlo.rev   # bit-reversed, as the Pocket wants it
python3 ../tools/make_release.py mirlo.rev tortuga.Mirlo_0.9.0.zip                        # the SD-card zip
```

The boot ROM and the geometry core's firmware are part of the bitstream: after changing either, or the register map, repeat the sequence (see [docs/geometry_core.md](docs/geometry_core.md)).

The design fills most of the device and its timing margins are thin. If a compile misses timing, try a few `SEED` values in `projects/mirlo.qsf`.

### Simulation and tests

| what | where |
|---|---|
| the whole SoC (CPU tests, the cube demo, the audio core, games) | `sim/mips_soc` (`./build.sh soc`, then `obj_soc/tb_soc <program.bin>`) |
| CPU and C library self-tests | `lang/mips/test` |
| MRDP C model vs RTL | `sim/mrdp` |
| MRDP setup microcode | `tools/mrdp_ucode.py test 2000` |
| geometry core RTL vs host build | `sim/geom_full/check_host_vs_rtl.sh` |
| vector unit | `sim/vpu` |
| audio DSP unit | `sim/audio` |
| SDRAM controller + PHY vs a chip model | `sim/mips_soc` (`./build.sh sdr`) |
| host unit tests | `make hosttest` in `lang/c/geom` |

## Other documents

* [docs/resolution.md](docs/resolution.md): the two video modes, and how to add another.

## License

Mirlo is released under the [Apache License 2.0](LICENSE). It includes and builds on third-party work under its own licenses: see [NOTICE](NOTICE).
