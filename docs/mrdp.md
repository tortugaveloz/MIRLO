# MRDP — Mirlo's N64-style rasterizer

MRDP is the rasterizer of the Mirlo SoC. It is designed the way the N64's
RDP is: 64-bit commands with the RDP's opcodes and bit layouts, a TMEM,
tiles, the N64 colour combiner and blender, and a 16-bit Z buffer. A game's
display-list model maps onto it directly.

It runs at the SoC clock (62.8 MHz) on the shared 16-bit SDRAM, through its
own LiteDRAM native port.

## Division of labour (as on the N64)

* **Geometry core = RSP.** It walks the GDL, transforms, lights and clips,
  and does triangle setup. It emits MRDP triangle commands. It also decodes
  N64 textures into 16-bit texels in its SDRAM texture cache (formats below).
* **MRDP = RDP.** It does edge walking, span setup, and per-pixel texture,
  combine, blend and Z. It also does all framebuffer, Z-buffer and TMEM
  memory traffic.

## Command stream

Commands are 64 bits wide: two 32-bit FIFO words, high word first. The
opcode sits in bits [61:56]. They arrive through two transports: the
`cmd_data` CSR (game CPU or geometry core) and the geometry core's CFU push
port. Unknown opcodes are **counted and skipped by length**, never executed
as something else.

| op   | command            | notes |
|------|--------------------|-------|
| 0x10–0x17 | TRI_V         | edges + plane factors + vertex values (below) |
| 0x18–0x1F | TRI_G         | edges + value, DaDx, DaDe per attribute (below) |
| 0x20 | TRI_R              | three raw vertices; MRDP does the whole setup (below) |
| 0x24 | texture rectangle  | N64 layout (+1 word S,T,DsDx,DtDy) |
| 0x27 | sync pipe          | no-op (in-order pipeline) |
| 0x28 | sync tile          | no-op |
| 0x29 | sync full          | completes once every earlier write is in DRAM; bumps `sync_count` |
| 0x2D | set scissor        | 10.2 fixed point, as N64 |
| 0x2E | set prim depth     | |
| 0x2F | set other modes    | N64 bit layout; subset honoured (below) |
| 0x32 | set tile size      | |
| 0x34 | load tile          | from the current texture image into TMEM |
| 0x35 | set tile           | 8 tiles; format, line, tmem, clamp/mirror/mask/shift |
| 0x36 | fill rectangle     | fill mode: colour or Z buffer clears, burst writes |
| 0x37 | set fill color     | |
| 0x38 | set fog color      | |
| 0x39 | set blend color    | |
| 0x3A | set prim color     | |
| 0x3B | set env color      | |
| 0x3C | set combine        | N64 bit layout, both cycles |
| 0x3D | set texture image  | SDRAM address + width (in texels) |
| 0x3E | set Z image        | |
| 0x3F | set color image    | |

N64-form triangles (0x08–0x0F) are skipped as unknown.

## Pixel formats

* **Colour image:** RGB565, the scan-out's format. Memory alpha reads as 1.
* **Z image:** 16-bit unsigned, larger = farther, compared per pixel. Z is
  iterated at s15.16 and truncated. The N64's 18-bit Z with deltaZ
  compression is not modelled.
* **TMEM texels:** 16-bit, decoded by the geometry core. Two formats:
  * **RGBA5551:** RGBA16, and CI via the TLUT.
  * **IA88:** IA4, IA8, IA16, I4 and I8, with no precision lost.

## TMEM

TMEM is 8 KiB, or 4,096 texels, so a 64×64 texture of any format fits.

It is split into **4 banks of 1,024 texels, interleaved by (s&1, t&1)**.
Texel (s,t) of a tile lives in bank `(t&1)*2 + (s&1)` at
`tmem + (t>>1)*(line/2) + (s>>1)`. Any 2×2 neighbourhood is then one read of
each bank. The N64 3-point filter needs three texels per pixel, at one pixel
per clock. LOAD TILE writes in that layout.

## Pipeline

1. **Command unit:** FIFO → decode → state registers / triangle staging RAM.
   Triangles are buffered whole before walking.
2. **Edge walker:** runs per scanline y in [max(YH, scissor), min(YL,
   scissor)), evaluated at the pixel-centre row. It walks the XH edge and
   the XM→XL edge (switching at YM), using the command's `left major` flag.
   A span is the pixels whose centre lies in [x_left, x_right), clipped to
   the scissor.
3. **Span setup:** attribute start = A + DaDe·(dy) + DaDx·(x0 − xh(y)). It
   uses one shared multiplier, sequentially over the attributes
   (R,G,B,A,S,T,W,Z).
4. **Span memory:** the Z span (if Z compare) and the colour span (if the
   blender reads memory) are burst-read into span buffers before the pixels
   run. Results are burst-written after. Spans are the unit of memory
   traffic.
5. **Pixel pipeline:** one pixel per clock in 1-cycle mode, one per two in
   2-cycle mode. The stages, in order:
   1. iterate (add DaDx);
   2. perspective: S·(1/W) and T·(1/W);
   3. tile: shift, mask, mirror, clamp;
   4. TMEM read (4 banks);
   5. 3-point filter;
   6. colour combiner: the N64's (A−B)·C+D for RGB and alpha, cycle 0 then
      cycle 1 on the same hardware;
   7. alpha compare;
   8. blender: P·A + M·B;
   9. Z compare / update;
   10. write to the span buffer.
6. **Sync full:** the memory unit counts writes issued and writes accepted.
   Sync full completes when they match and the pipeline is empty.

## Other modes honoured

* **Cycle type:** 1-cycle, 2-cycle and fill; copy is treated as 1-cycle.
* **Texture filter:** point or 3-point "bilinear".
* **Perspective texture enable.**
* **Z:** compare and update. Z modes:
  * opaque;
  * interpenetrating (treated as opaque);
  * translucent;
  * decal: compare with a small bias.
* **Alpha compare:** the threshold is the blend colour's alpha.
* **Blending:** force blend, image read enable, and the blender mux selects
  for both cycles.
* **Ignored:** the coverage/AA bits and dither.

## Design rules

* **Done means in DRAM.** Only SYNC FULL signals completion, and only after
  every write is accepted by LiteDRAM. There is no "busy" flag to misread.
* **No hidden staging buffers.** LOAD TILE copies textures into TMEM inside
  the command stream, in order. Nothing is DMA'd later from a buffer the
  CPU may already be reusing.
* **Top-down framebuffer**, which is the scan-out's layout. The Y flip lives
  in the geometry core only.
* **State is per register**, never a shared bitfield that one command
  rewrites wholesale.
* **Fixed point with documented widths everywhere**, checked against a
  bit-exact C model before any RTL.
* **The FIFO owns flow control.** A full FIFO stalls the CFU push. CSR
  writers poll a level. Dropped words are counted.
* **The C model is the reference.** It is bit-exact with the RTL, so host
  and hardware cannot quietly diverge.

## Integration

* **Gateware:** `litex/mrdp.py` wraps `rtl/mrdp/mrdp_top.v`. It provides:
  * the command FIFO, on the `cmd_data` CSR;
  * the status CSRs `cmd_status`, `cmd_dropped`, `sync_count`, `load_count`
    and `status`;
  * its own LiteDRAM native port, addressed as byte address − main-RAM
    base, in 32-bit words.
* **Geometry core** (`lang/c/geom`, built with `GEOM_MRDP`): `geom_mrdp.h`
  and `geom_mrdp_hw.c` emit the commands. Triangle setup is
  `lang/c/mrdp/mrdp_setup.h`: integer only, with no divides (a Newton
  reciprocal on 32×32→64 multiplies).
* **Game CPU** (`lang/c/game/frame.c`): each frame's GDL starts with the
  clears (`mrdp_frame.h`, as `GDL_RAW`) and ends with SYNC FULL. The flip
  waits for `sync_count` to count it. After `frame_init()` only the
  geometry core writes the FIFO.

## Triangle commands

MRDP's RTL takes three triangle forms. `lang/c/mrdp/mrdp_setup.h` documents
each word by word.

* **TRI_R (0x20, flags in w0[21:19]):** three sorted, y-snapped vertices,
  with each vertex's raw fields (colour 0..1, s, t, w, z 0..1). MRDP does
  the whole setup:
  * area;
  * three edge slopes (reciprocals) and the x starts;
  * 1/area as four plane factors with a per-triangle exponent;
  * W normalisation and the colour / Z conversions;
  * the gradients (as TRI_V).

  This is the common form.
* **TRI_V (0x10|flags):** the N64 edge words, four plane factors
  F = d · 2^46 / area, kx, ky, and every attribute's value at the three
  sorted vertices. MRDP computes the gradients itself (~15 cycles per
  attribute on its multiplier; `mrdp_vattr()` is the arithmetic). Decals use
  it, with the bias on the vertex Z.
* **TRI_G (0x18|flags):** the edges plus value, DaDx and DaDe per attribute,
  all set up in C. It is used for slivers whose factors do not fit 31 bits.

All three forms go through a 32-word staging RAM and one write port into
the attribute registers.

### The setup engine is microcode

TRI_R's setup runs on a small microcoded machine:
* 8 registers and one adder-based ALU;
* MRDP's existing multiplier and 64-bit shifter;
* the vertex RAM as storage;
* a ROM block holding the program.

`tools/mrdp_ucode.py` holds the program, its assembler, a cycle-accurate
model and a differential test against the C spec. `rtl/mrdp/mrdp_ucode_rom.v`
is generated from it. Setup takes ~160 cycles per triangle on average.

A straight FSM with one state per step costs about twice the area. Every
step loads its own registers from its own expressions, so the cost is
operand muxes, not arithmetic.

**Timing rule:** the ROM's output drives the datapath directly, so nothing
slow may sit in front of the ALU's adder in the same cycle. That means:
* the shifter's output and its saturation are registered (SH is available
  two instructions after a shl, SHS three);
* no ALU operand comes from the unsigned product (its correction adder);
* the exponent is clamped at DONE.

### Numerics

* **Reciprocals, as the N64's RSP does them.** A 512-entry, 16-bit seed
  ROM shaped like the RSP's VRCP table, plus one Newton step. Entry
  i = min(0xFFFF, ((2^34 / (512 + i) + 1) >> 8) − 2^16).
  `tools/mrdp_ucode.py` generates `rtl/mrdp/mrdp_seed_rom.v` from it and
  checks it against the formula.
* **W is 2.30 (1.0 = 2^30), not 16.16.**
  * With 1.0 = 2^16, a far vertex's normalised W' has about a dozen
    significant bits, and its per-pixel gradient rounds to whole units.
  * Over the rows from a big triangle's off-screen anchor, that error
    reaches ~0.25 % of W — more than a texel at large S. Textures on big
    surfaces visibly swim.
  * The N64 keeps W large as well: its RDP normalises W from the integer
    bits.
* **Edge grow ("crack fill", `AA_EN`).** It closes sub-pixel cracks between
  adjacent surfaces.
  * **Which triangles:** the geometry core sets AA_EN on opaque,
    depth-writing, non-decal, non-2D triangles.
  * **Which edges:** TRI_R's microcode marks the edges that may grow
    (GRW; `mrdp_grow_mask` in `mrdp_setup.h`). Only edges the triangle is
    about a pixel thick across grow (2·area ≥ |dx| + |dy|), so a sliver's
    band never extrapolates colour and depth along it.
  * **How far:** a growing edge moves out 1/8 px across it. Along a row that
    is (1 + |DxDy|) / 8 px (`mrdp_grow_amount`), capped at 1/4 px, because a
    row walker has no third edge to stop a near-horizontal edge's shift.
  * **Where:** spans only; y stays in quarter pixels, as on the N64.
    TRI_V and TRI_G triangles do not grow.
* **The N64's coverage (`MRDP_COVERAGE`, off by default)** is kept for
  anti-aliasing experiments. Each row is four subscanlines. Each edge sits at
  the row centre plus (j − 2) quarter slopes, rounded to 1/8 px with the
  N64's sticky bit. There are two samples per subscanline, in a
  checkerboard, and a pixel is drawn when any sample is covered. Z moves to
  the first covered sample, as the N64's offx/offy do.
  * **Missing against the N64:** the RDP's coverage blend, 3-bit coverage in
    the framebuffer, and the VI's divot and dither filters.
  * **Building it:** use `+define+MRDP_COVERAGE` for the RTL and
    `-DMRDP_COVERAGE` for the model (in `sim/mrdp`, `VDEFS` / `TB_CFLAGS`).
    `rm -rf obj_dir` first: Verilator's make does not rebuild when only the
    flags change.
  * **Cost:** ~+600 ALMs and 2 DSPs.
* **Attribute storage.** The attribute row values e[] and their row steps
  ade[] are LUT RAM (MLAB), read one at a time. The per-pixel steps adx[]
  stay in flip-flops, because every pixel steps all eight attributes at
  once.
* **Build profile** (`projects/*.qsf`):
  * MRDP: `MRDP_NO_2CYCLE`, `MRDP_NO_TEXRECT` and `MRDP_NO_PRIMENV` drop
    features the geometry pipeline does not emit.
  * The geometry CFU: `SU_NO_EDGE` and `SU_NO_BLEND`.

## How it is verified

| what | where | checks |
|---|---|---|
| C model | `lang/c/mrdp/test_mrdp.c` | scene assertions |
| RTL vs model | `sim/mrdp` (`tb_mrdp <capture>`) | all memory identical; random command streams from `test_mrdp_rand` |
| setup microcode | `tools/mrdp_ucode.py test N` | N random TRI_R commands: microcode model vs the C spec |
| geometry RTL vs host | `sim/geom_full/check_host_vs_rtl.sh` | word-identical command streams and identical frames (model linked into the bench) |
| in the SoC | `litex/sim_mrdp.py` + `sim/soc_firmware/mrdp_main.c` | real CSR bus, FIFO, LiteDRAM port + SDRAM model: colour/depth hashes == model |

## Area and timing notes (Cyclone V)

* **Byte-enable RAMs need 8/9/10-bit lanes.** 16-bit lanes build line
  buffers from flip-flops, so use one memory per lane.
* **LUT RAM inference.** `(* ramstyle = "MLAB, no_rw_check" *)` on a plain
  `reg [31:0] m [0:7]` works, with one clocked write and `assign q = m[a]`.
  It infers altdpram.
* **Look for ALMs in register files first.** Every multiplier already maps
  to a DSP. The ALMs to win are in flip-flop register files read one word at
  a time.
* **Register quasi-static selections.** Anything indexed by quasi-static
  state (`tile[p_tile]`) into the pixel pipe is a timing path, so register
  the selection when the primitive starts.
* **Split operand muxes from arithmetic** in the combiner and the blender,
  and split the TMEM read from the filter.
* **Measure area with standalone fits** (`VIRTUAL_PIN ON`, map + fit, a few
  minutes). Use a full compile only to confirm.
* **Measure a feature's area by deleting its code.** Tying an enable to 0
  often leaves the logic in place.
* **Watch shift-register inference.** Quartus folds pipeline registers into
  RAM delay lines (altshift_taps). When the head of one is the end of a long
  path, the M10K's input setup fails it. Put
  `(* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *)` on
  that one register.
