#!/usr/bin/env python3

#
# This file is part of LiteX-Boards.
#
# Copyright (c) 2023 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

# ./analog_pocket.py --uart-name=jtag_uart --build --load
# litex_term jtag --jtag-config=openocd_usb_blaster.cfg

# Set up the import paths for the LiteX packages
import os

import vendor
from csr import APFID, APFRTC, APFAudio, APFBridge, APFInput, APFInteract, APFVideo
from litex.soc.cores.uart import UART
from mrdp import MRDPCore
from vpu_fixed import VPUFixedCfu
from mailbox import Mailbox
from replaced_components import (
    AS4C32M16Pocket,
    FixedVideoTimingGenerator,
    HalfRateGENSDRAMPocketPHY,
    UARTPHYMultiplexer,
    VideoPocketPHY,
)
from litex.soc.integration.soc import SoCRegion

from litex.soc.interconnect import wishbone

from migen import *

from litex.gen import *

import verilog_platform as analogue_pocket

from litex.soc.integration.soc_core import *
from litex.soc.integration.builder import *

from litex.build.io import DDROutput

# CRG ----------------------------------------------------------------------------------------------

# sys = 11 x the 5.712 MHz pixel clock (2026-09-25; was 10 x = 57.12 MHz).
# An integer multiple keeps sys/sys2x/vid one synchronous PLL group
# (core_constraints.sdc); the fractional PLL runs its VCO at 110 x = 628.32
# MHz. sys2x = 125.664 MHz drives the SDRAM. 68.544 MHz (12 x) is worked out
# on the clk68 branch; the PLL is target/pocket/mf_pllbase/mf_pllbase_0002.v.
CLOCK_SPEED = 62.832e6
PIX_CLK = 5.712e6


class _CRG(LiteXModule):
    def __init__(self, platform: analogue_pocket.Platform):
        # `rst` is a magic CRG signal that is automatically wired to the output of the SoC reset
        self.rst = Signal()
        # LiteX expects a `sys` clock domain, so we can't rename it
        self.cd_sys = ClockDomain()
        # LiteX also expects a `sys2x` clock domain when using double rate SDRAM, and it can't be renamed
        self.cd_sys2x = ClockDomain()
        self.cd_sys2x_90deg = ClockDomain()
        self.cd_vid = ClockDomain()

        reset_pin = platform.request("reset")

        clk_sys = platform.request("clk_sys")
        self.comb += self.cd_sys.clk.eq(clk_sys)
        self.comb += self.cd_sys.rst.eq(self.rst | reset_pin)

        clk_sys2x = platform.request("clk_sys2x")
        self.comb += self.cd_sys2x.clk.eq(clk_sys2x)
        # sys2x's reset comes from a register of its own clock: from the
        # sys-domain reset straight to every sys2x register (the SDRAM PHY's
        # pad registers included) it has only one sys2x period.
        self.cd_sys2x_rl = ClockDomain(reset_less=True)
        self.comb += self.cd_sys2x_rl.clk.eq(clk_sys2x)
        rst_sys2x = Signal(reset=1)
        self.sync.sys2x_rl += rst_sys2x.eq(self.rst | reset_pin)
        self.comb += self.cd_sys2x.rst.eq(rst_sys2x)

        clk_sys2x_90deg = platform.request("clk_sys2x_90deg")
        self.comb += self.cd_sys2x_90deg.clk.eq(clk_sys2x_90deg)

        clk_vid = platform.request("clk_vid")
        self.comb += self.cd_vid.clk.eq(clk_vid)
        # Without this, cd_vid never resets after the FPGA's initial
        # configuration -- "Reset core" (used constantly for JTAG SFL
        # firmware uploads) only clears cd_sys/cd_sys2x above, never
        # cd_vid. Found on real hardware 2026-09-14: the video timing
        # generator (in "vid", see analogue_pocket.py's video_framebuffer_vtg)
        # and APFVideo's vblank tracking (litex/csr.py) worked correctly
        # immediately after a fresh core relaunch (frame_counter
        # incrementing, HDMI showing real framebuffer content), but got
        # permanently wedged (frame_counter frozen, vblank never observed
        # again, solid black HDMI) the moment any soft "Reset core" was
        # used afterward -- with NO way to recover short of a full
        # Quit+Relaunch (full FPGA reconfiguration), since nothing else
        # ever reset that domain's state. Verified via a from-scratch
        # diagnostic firmware (no rasterizer/geom, just a raw CPU memset +
        # video_framebuffer_dma_* CSR writes) that this reproduces with
        # zero rendering pipeline involved -- it is purely this missing
        # reset, not a rasterizer/geom/DRAM-arbitration bug.
        self.comb += self.cd_vid.rst.eq(self.rst | reset_pin)

        # # #

        # SDRAM clock
        sdram_clk = clk_sys2x_90deg
        self.specials += DDROutput(1, 0, platform.request("sdram_clock"), sdram_clk)


# BaseSoC ------------------------------------------------------------------------------------------


class BaseSoC(SoCCore):
    def __init__(self, sys_clk_freq, **kwargs):
        platform = analogue_pocket.Platform()

        # CRG --------------------------------------------------------------------------------------
        self.crg = _CRG(platform)

        # SoCCore ----------------------------------------------------------------------------------
        SoCCore.__init__(
            self, platform, sys_clk_freq, ident="LiteX SoC on Analog Pocket", **kwargs
        )

        self.add_constant("DEPLOYMENT_PLATFORM", "openfpga")
        self.add_constant("DEPLOYMENT_TARGET", "pocket")

        # Allow booting from the first address in SDRAM
        self.add_constant("ROM_BOOT_ADDRESS", 0x40000000)
        # self.add_constant("SDRAM_TEST_DISABLE", 1)

        # SDR SDRAM --------------------------------------------------------------------------------
        if not self.integrated_main_ram_size:
            self.sdrphy = HalfRateGENSDRAMPocketPHY(
                platform.request("sdram"), sys_clk_freq
            )
            self.add_sdram(
                "sdram",
                phy=self.sdrphy,
                module=AS4C32M16Pocket(sys_clk_freq, "1:2"),
                # l2_cache_size = kwargs.get("l2_size", 8192)
                # Disable L2 as it seems to not being used for reads, and it is causing 0x2000 bytes to be not written
                # on file read
                l2_cache_size=0,
            )

        self.submodules.videophy = VideoPocketPHY(platform.request("vga"))
        timings = {
            "pix_clk": PIX_CLK,
            "h_active": 268,
            "h_blanking": 72,  # line total 340
            "h_sync_offset": 8,
            "h_sync_width": 32,
            "v_active": 240,
            "v_blanking": 40,  # Max 280
            "v_sync_offset": 1,
            "v_sync_width": 8,
        }
        
        # Manual add_video_framebuffer() equivalent (not the stock LiteX
        # helper, which hardcodes VideoFrameBuffer's default fifo_depth of
        # 64KiB). It was doubled to 128KiB 2026-09-13 and is back at 64KiB
        # since 2026-09-23 (see the VideoFrameBuffer call). The reasoning
        # for the doubling, kept for the record: the plain LiteDRAM
        # crossbar arbiter (litedram/core/crossbar.py) is a flat
        # round-robin across ALL masters (video DMA, rasterizer, geom core,
        # game CPU) with no priority/QoS mechanism, so heavier per-frame
        # DRAM traffic from the rasterizer/geom (more triangles = more triangle-
        # setup memory requests) proportionally starves the video DMA
        # reader's fair share. VideoFrameBuffer's FSM gates
        # vtg_sink.ready on source.valid (see video.py's "RUN" state), so
        # a FIFO underflow doesn't just show a stale/glitched frame -- it
        # stalls the video timing generator itself, which is why this
        # presents as a hard freeze (sync stops, not just pixel data)
        # rather than a recoverable glitch. Observed on hardware: a
        # 12-tri/frame rotating cube ran
        # 2000 frames with no issue, but a 20-tri/frame rotating
        # icosahedron froze solid within a few hundred frames -- consistent
        # with FIFO underflow probability scaling with contention. Doubling
        # the FIFO is a mitigation (more margin before underflow), not a
        # guarantee against arbitrarily heavy contention -- a real fix
        # would need priority/reservation in the crossbar arbiter itself.
        from litex.soc.cores.video import VideoFrameBuffer
        # Fixed timings (2026-09-26): the resolution is no longer selectable,
        # and the stock generator's programmable registers cost ~200 FFs.
        # The second resolution: the N64's own 320 x 240, same pixel clock,
        # 360 x 264 per frame (60.1 Hz; the 268 set is 340 x 280, 60.0 Hz).
        # A game's header picks it (lang/c/game/frame.c frame_set_video,
        # video.json scaler slot 1).
        timings320 = dict(timings,
            h_active = 320, h_blanking = 40, h_sync_offset = 8, h_sync_width = 24,
            v_active = 240, v_blanking = 24, v_sync_offset = 1, v_sync_width = 8)
        vfb_vtg = FixedVideoTimingGenerator(timings, timings320)
        vfb_vtg = ClockDomainsRenamer("vid")(vfb_vtg)
        self.add_module(name="video_framebuffer_vtg", module=vfb_vtg)
        vfb_base = self.mem_map.get("video_framebuffer", None)
        if vfb_base is None:
            self.bus.add_region("video_framebuffer", SoCRegion(
                origin = 0x40c00000, size = 0x800000, linker = True))
            vfb_base = self.bus.regions["video_framebuffer"].origin
        vfb_dram_port = self.sdram.crossbar.get_port()
        # Plain round-robin DRAM arbitration, on purpose. Giving the scan-out
        # DMA absolute priority (LiteDRAMCrossbar priority_master_id) starved
        # the CPU of DRAM, and the bounded-streak variant of it wedged the
        # scan-out DMA itself (permanently black HDMI). Simulate any QoS
        # scheme before trying it on hardware.
        #
        # Video FIFO: LiteX's stock 64 KiB. 128 KiB would cost 128 of the
        # device's 308 M10K blocks. If scan-out ever freezes under heavy DRAM
        # traffic, this is the first suspect.
        vfb = VideoFrameBuffer(vfb_dram_port,
            hres = timings["h_active"], vres = timings["v_active"], base = vfb_base, format = "rgb565",
            fifo_depth = int(os.environ.get("VFB_FIFO_DEPTH", "65536")),   # bytes
            clock_domain = "vid",
            clock_faster_than_sys = vfb_vtg.video_timings["pix_clk"] >= self.sys_clk_freq,
        )
        self.add_module(name="video_framebuffer", module=vfb)
        self.comb += vfb_vtg.source.connect(vfb.vtg_sink)
        self.comb += vfb.source.connect(self.videophy.sink)
        self.comb += self.videophy.slot.eq(vfb_vtg.mode_r)      # scaler slot 1 = 320 x 240
        self.add_constant("VIDEO_FRAMEBUFFER_BASE", vfb_base)
        self.add_constant("VIDEO_FRAMEBUFFER_HRES", timings["h_active"])
        self.add_constant("VIDEO_FRAMEBUFFER_VRES", 240)
        self.add_constant("VIDEO_FRAMEBUFFER_DEPTH", vfb.depth)

        self.add_constant("MAX_DISPLAY_WIDTH", timings["h_active"])
        self.add_constant("MAX_DISPLAY_HEIGHT", timings["v_active"])

        # CSR definitions --------------------------------------------------------------------------
        self.add_module("apf_audio", APFAudio(platform))
        self.add_module("apf_bridge", APFBridge(platform))
        # APF chip ID and RTC: no firmware reads them (only the fungus
        # example read the RTC). Their pads stay requested so the litex
        # module keeps the ports core_top.sv connects.
        platform.request("apf_id")
        self.add_module("apf_input", APFInput(platform))
        # hal/controller.c: the button map. Slot 0 = A, B, Z, Start, L, R and
        # slot 1 = C-up/down/left/right (5 bits each), stick [21:20], D-pad
        # [23:22], Show FPS [24], Start = Select+Start [25]; each setting also has its own address
        # 0x10000108.. (interact.json), which writes just its field.
        interact_fields = {2 + i: (0, 5 * i, 5) for i in range(6)}
        interact_fields.update({8 + i: (1, 5 * i, 5) for i in range(4)})
        interact_fields.update({12: (1, 20, 2), 13: (1, 22, 2), 14: (1, 24, 1), 15: (1, 25, 1)})
        self.add_module("apf_interact", APFInteract(platform, slots=2, fields=interact_fields))
        platform.request("apf_rtc")
        self.add_module("apf_video", APFVideo(self, timings["v_active"]))

        # MRDP (docs/mrdp.md): an N64-style rasterizer (it takes the RDP's
        # command format) with its own LiteDRAM crossbar port and a
        # 2048-word command FIFO, fed by the geometry core.
        self.mrdp = MRDPCore(platform, dram_port=self.sdram.crossbar.get_port(),
                             dram_base=self.bus.regions["main_ram"].origin,
                             cmd_fifo_depth=int(os.environ.get("MRDP_CMD_FIFO_DEPTH", "2048")))

        # Geometry subsystem: 2nd RISC-V core (own FPU) + cross-core mailbox
        self.add_geometry_subsystem(platform)

        # Audio subsystem: 3rd RISC-V core (rv32i + Zmmul, no caches) with
        # its own IMEM/DMEM. AUDIO_CORE=0 builds the SoC without it.
        if os.environ.get("AUDIO_CORE", "1") == "1":
            self.add_audio_subsystem(platform)

        self.add_uart(platform)

        # The exported "example" Wishbone slave was never connected in
        # core_top.sv (an access there would hang the bus). Its pads stay as
        # ports, idle.
        example_pads = platform.request("wishbone")
        self.comb += [example_pads.cyc.eq(0), example_pads.stb.eq(0), example_pads.we.eq(0),
                      example_pads.adr.eq(0), example_pads.dat_w.eq(0), example_pads.sel.eq(0),
                      example_pads.cti.eq(0), example_pads.bte.eq(0)]

        apf_bridge_master = wishbone.Interface()

        self.bus.add_master("apf_bridge_master", apf_bridge_master)

        self.comb += apf_bridge_master.connect_to_pads(
            platform.request("wishbone_master"), mode="slave"
        )

    # Geometry subsystem --------------------------------------------------------
    #
    # A second, deliberately small RISC-V core (rv32im: NO hardware FPU, no
    # RVC, no data cache beyond its own private L1) that runs the SM64 vertex
    # pipeline in software: it walks the display list the game CPU builds in
    # SDRAM, does the matrix stack / vertex transform / viewport map /
    # triangle setup, and pushes N64 triangle commands to MRDP.
    # fp32 vec4/mat4 math is pure-integer S15.16 fixed point underneath
    # (lang/c/geom/geom_fixed.c/geom_vecmath.c) -- `float` stays the C-level
    # type at call sites, it just never reaches a real hardware or soft-float
    # op. This matches the real N64: the RSP (this core's role) had no FPU
    # at all, only the main VR4300 CPU did (see geom_fixed.h's header for
    # the full rationale). Per-vertex transform (load_vertices()'s matrix
    # multiply, the hottest per-vertex cost) is additionally accelerated by
    # a parallel fixed-point CFU (self.vpu_fixed below, rtl/vpu/
    # Vpu4DFixed.v, its own separate S17.10 format -- see
    # docs/geometry_core.md). The two cores hand off frames through the
    # Mailbox doorbell/IRQ.
    #
    # The netlist's generator is GenCoreGeomCfu.scala in the VexRiscv
    # submodule (vendor patches: litex/vendor/patches).
    #
    # It is a standalone VexRiscv netlist (renamed to VexRiscvGeom to avoid a
    # module-name clash with the SMP cluster's internal `VexRiscv`), NOT a
    # second core in the VexRiscv-SMP cluster: --cpu-count=2 would force
    # 64-bit / 8 KB / 2-way L1s + coherent DMA + a 128-bit LiteDRAM port and an
    # sbt-regenerated SMP+FPU netlist, far more than the Cyclone V can spare.
    # The geometry core boots from a private ROM and keeps its working set in a
    # separate private RAM. They MUST be split: a byte-writable RAM
    # (`add_ram` with contents) inferred on this Cyclone V loses its
    # `$readmemh` initialisation -- Quartus splits it into four byte-lane
    # altsyncram blocks and the 32-bit-wide init is dropped, so the core boots
    # into a scratchpad full of zeros (= illegal instruction -> trap loop, and
    # the geometry pipeline never responds). A read-only ROM infers as a
    # single 32-bit block and keeps its init, exactly like the main CPU ROM.
    GEOM_ROM_BASE = 0x2000_0000
    # 32 KiB: geom .text + .rodata (+ .data LMA), as ROM A (16 KiB, also on
    # the data bus) + ROM B (16 KiB, instruction fetch only). The firmware is
    # built -O2 (sim/geom_full: ~15 % fewer cycles than -Os).
    GEOM_ROM_SIZE = 0x0000_8000
    GEOM_ROM_WINDOW = 0x0000_8000
    GEOM_RAM_BASE = 0x2000_8000
    GEOM_RAM_SIZE = 0x0000_4000  # 16 KiB: geom .data + .bss + stack (uninit)
    # Back-compat alias for the reset vector.
    GEOM_SRAM_BASE = GEOM_ROM_BASE

    def add_geometry_subsystem(self, platform):
        # Geometry-core ROM, pre-loaded at synthesis time with
        # lang/c/geom/build/geom.bin if it has been built (two-pass: build the
        # gateware once to emit csr.h, `make` in lang/c/geom, rebuild the
        # gateware); otherwise a bare `j .` spin loop so the core is
        # well-defined either way. The core's reset vector points at
        # GEOM_ROM_BASE.
        geom_fw = os.path.join(
            os.path.dirname(__file__), "..", "lang", "c", "geom", "build", "geom.bin"
        )
        if os.path.exists(geom_fw):
            from litex.soc.integration.common import get_mem_data
            geom_contents = get_mem_data(geom_fw, endianness="little")
        else:
            geom_contents = [0x0000006f]  # RISC-V `jal x0, 0` == infinite self-loop
        # ROM and RAM sit on the geometry core's OWN bus, not the SoC's.
        # Measured 2026-09-24 (sim/geom_full, BUS_WAIT calibrated to the
        # hardware's GEOMPROF): per game frame the core makes ~460 K I-cache
        # refill reads from its ROM and ~260 K write-through stores to its RAM.
        # Through the shared interconnect -- one master port for both of its
        # buses, arbitrated against everything else -- that was ~1.1 M of the
        # ~5.3 M cycles a frame costs. Locally, with burst-capable SRAMs, the
        # I-cache refills stream a word per cycle. Only SDRAM and CSR accesses
        # still leave through the main bus. The game CPU keeps a main-bus port
        # into both (it reads geom RAM, e.g. geom_tris_emitted), arbitrated
        # with the core's own.
        def _local_bus():
            return wishbone.Interface(data_width=32, address_width=32, addressing="word", bursting=True)
        geom_rom_bus = _local_bus()
        # The core fetches instructions straight from the ROM through a second
        # read port (GenCoreGeomCfu.scala GEOM_IBUS=simple: no instruction
        # cache). The ROM is on-chip, so the 8 KiB I-cache only duplicated it,
        # and its misses were ~6 % of a frame (sim/geom_full, JRB attract
        # frame 2.199 -> 2.053 M cycles); the cache's RAM and logic go too.
        # Two memories: ROM A (16 KiB) on the iBus and the Wishbone port as
        # before, ROM B (8 KiB) on the iBus only -- the linker (lang/c/geom/
        # geom.ld .text_b) puts nothing but code there. One 6144-deep ROM with
        # two read ports cannot be sliced into M10Ks bit-wise (true dual port
        # stops at x20): Quartus built a 6:1 read mux per bit and port in logic
        # and the design no longer fit (1869 of 1848 LABs; 1859 with ROM B
        # still on the Wishbone side too).
        words = self.GEOM_ROM_SIZE // 4
        geom_contents = list(geom_contents) + [0] * (words - len(geom_contents))
        geom_rom_a = Memory(32, 4096, init=geom_contents[:4096], name="geom_rom_a")
        geom_rom_b = Memory(32, words - 4096, init=geom_contents[4096:], name="geom_rom_b")
        self.geom_rom = wishbone.SRAM(geom_rom_a, bus=geom_rom_bus, read_only=True)
        # synchronous read, the same style as the Wishbone port's (`q <= mem[a]`)
        # -- mixed with an address-registered port, Quartus built the ROM
        # from registers instead of M10Ks
        geom_rom_iport_a = geom_rom_a.get_port(mode=READ_FIRST)
        geom_rom_iport_b = geom_rom_b.get_port(mode=READ_FIRST)
        self.specials += geom_rom_b, geom_rom_iport_a, geom_rom_iport_b
        # The RAM is the core's tightly coupled data memory (GenCoreGeomCfu.scala
        # GEOM_DTCM=1): loads and stores reach it from the execute stage in one
        # cycle, bypassing the write-through D-cache -- no refills, no Wishbone
        # store per `sw` (sim/geom_full, JRB attract frame -4 %). The SoC's
        # port shares the one RAM port in the cycles the core leaves it idle.
        # rtl/geom/GeomTcmRam.v: an altsyncram, true dual port (see below)
        assert self.GEOM_RAM_SIZE == 0x4000
        geom_rom_soc = wishbone.Interface(data_width=32, address_width=32, addressing="word")
        geom_ram_soc = wishbone.Interface(data_width=32, address_width=32, addressing="word")
        self.bus.add_slave(name="geom_rom", slave=geom_rom_soc,
                           region=SoCRegion(origin=self.GEOM_ROM_BASE, size=self.GEOM_ROM_WINDOW, mode="rx"))
        self.bus.add_slave(name="geom_ram", slave=geom_ram_soc,
                           region=SoCRegion(origin=self.GEOM_RAM_BASE, size=self.GEOM_RAM_SIZE, mode="rwx"))

        # Cross-core mailbox stays a CSR peripheral (both cores reach it).
        self.add_module("mailbox", Mailbox())

        # Parallel fixed-point matrix-vector engine: attached to the geometry
        # core as a CFU (custom function unit) -- see docs/geometry_core.md
        # and rtl/vpu/Vpu4DFixed.v. Only load_vertices()'s gm_matvec routes
        # through it (its own S17.10 format, chosen to fit one Cyclone V DSP
        # block per multiply); every other fp32 op on this core is plain
        # S15.16 software fixed point (geom_fixed.c/geom_vecmath.c), not the
        # CFU and not an FPU -- there is no FPU on this core at all. The game
        # CPU has no path here.
        self.vpu_fixed = VPUFixedCfu(platform)
        self.comb += [
            self.mrdp.su_valid.eq(self.vpu_fixed.out_valid),
            self.mrdp.su_data.eq(self.vpu_fixed.out_data),
            self.vpu_fixed.out_ready.eq(self.mrdp.su_ready),
        ]

        # Geometry core buses. iBus: code, only ever from its ROM. dBus: its
        # ROM (.rodata), its RAM, else the SoC bus (SDRAM, CSRs) -- the one
        # master port it has there. ROM 0x2000_0000 (32 KiB in a 32 KiB
        # window), RAM 0x2000_8000 (16 KiB, the core's TCM).
        geom_dbus = _local_bus()
        geom_dbus_rom = _local_bus()
        geom_bus  = wishbone.Interface(data_width=32, address_width=32, addressing="word")
        rom_page = self.GEOM_ROM_BASE >> 15             # its 32 KiB window
        self.geom_decoder = wishbone.Decoder(geom_dbus, [
            (lambda a: a[13:30] == rom_page, geom_dbus_rom),
            (lambda a: a[13:30] != rom_page, geom_bus),     # the RAM never reaches the D-cache
        ])
        self.geom_rom_arbiter = wishbone.Arbiter([geom_dbus_rom, geom_rom_soc], geom_rom_bus)
        # iBus (IBusSimplePlugin): a command every cycle (ready = 1), its
        # instruction the next -- exactly the port's one-cycle read latency
        geom_ib_valid = Signal()
        geom_ib_pc = Signal(32)
        geom_ib_rsp = Signal()
        geom_ib_sel = Signal()               # ROM B's word, registered with the read
        geom_ib_inst = Signal(32)
        self.comb += [
            geom_rom_iport_a.adr.eq(geom_ib_pc[2:14]),
            geom_rom_iport_b.adr.eq(geom_ib_pc[2:2 + len(geom_rom_iport_b.adr)]),
            geom_ib_inst.eq(Mux(geom_ib_sel, geom_rom_iport_b.dat_r, geom_rom_iport_a.dat_r)),
        ]
        self.sync += [geom_ib_rsp.eq(geom_ib_valid), geom_ib_sel.eq(geom_ib_pc[14])]
        # dTcm: the RAM's port A, address/data from the core's execute stage;
        # the read lands the next cycle, when the load is in the memory stage,
        # and holds while the port is idle (read enable = the port's enable)
        # -- so a stalled memory stage still sees it. Port B is the SoC's
        # (true dual port: each M10K 4K x 2, no muxes in logic).
        tcm_en    = Signal()
        tcm_adr   = Signal(32)
        tcm_we    = Signal()
        tcm_wdat  = Signal(32)
        tcm_mask  = Signal(4)
        ram_words = log2_int(self.GEOM_RAM_SIZE // 4)
        soc_go    = Signal()
        tcm_rdat  = Signal(32)
        self.comb += soc_go.eq(geom_ram_soc.cyc & geom_ram_soc.stb & ~geom_ram_soc.ack)
        self.sync += geom_ram_soc.ack.eq(soc_go)
        self.specials += Instance("GeomTcmRam",
            i_clk     = ClockSignal("sys"),
            i_a_en    = tcm_en,
            i_a_adr   = tcm_adr[2:2 + ram_words],
            i_a_we    = Replicate(tcm_we, 4) & tcm_mask,
            i_a_dat_w = tcm_wdat,
            o_a_dat_r = tcm_rdat,
            i_b_adr   = geom_ram_soc.adr[:ram_words],
            i_b_we    = Replicate(soc_go & geom_ram_soc.we, 4) & geom_ram_soc.sel,
            i_b_dat_w = geom_ram_soc.dat_w,
            o_b_dat_r = geom_ram_soc.dat_r,
        )
        platform.add_source(os.path.join(os.path.dirname(__file__), "..", "rtl", "geom", "GeomTcmRam.v"))
        self.bus.add_master("geom_cpu", geom_bus)

        geom_ext_irq = Signal(32)
        self.comb += geom_ext_irq[0].eq(self.mailbox.geom_irq)

        self.specials += Instance(
            "VexRiscvGeom",
            i_clk                    = ClockSignal("sys"),
            i_reset                  = ResetSignal("sys"),
            i_externalResetVector    = C(self.GEOM_SRAM_BASE, 32),
            i_timerInterrupt         = 0,
            i_softwareInterrupt      = 0,
            i_externalInterruptArray = geom_ext_irq,

            # Parallel fixed-point matrix-vector CFU
            o_CfuPlugin_bus_cmd_valid               = self.vpu_fixed.cmd_valid,
            i_CfuPlugin_bus_cmd_ready               = self.vpu_fixed.cmd_ready,
            o_CfuPlugin_bus_cmd_payload_function_id = self.vpu_fixed.cmd_function_id,
            o_CfuPlugin_bus_cmd_payload_inputs_0    = self.vpu_fixed.cmd_inputs_0,
            o_CfuPlugin_bus_cmd_payload_inputs_1    = self.vpu_fixed.cmd_inputs_1,
            i_CfuPlugin_bus_rsp_valid               = self.vpu_fixed.rsp_valid,
            o_CfuPlugin_bus_rsp_ready               = self.vpu_fixed.rsp_ready,
            i_CfuPlugin_bus_rsp_payload_outputs_0   = self.vpu_fixed.rsp_outputs_0,

            o_iBus_cmd_valid         = geom_ib_valid,
            i_iBus_cmd_ready         = 1,
            o_iBus_cmd_payload_pc    = geom_ib_pc,
            i_iBus_rsp_valid         = geom_ib_rsp,
            i_iBus_rsp_payload_error = 0,
            i_iBus_rsp_payload_inst  = geom_ib_inst,

            o_dTcm_enable            = tcm_en,
            o_dTcm_address           = tcm_adr,
            o_dTcm_write_enable      = tcm_we,
            o_dTcm_write_data        = tcm_wdat,
            o_dTcm_write_mask        = tcm_mask,
            i_dTcm_read_data         = tcm_rdat,

            o_dBusWishbone_ADR       = geom_dbus.adr,
            o_dBusWishbone_DAT_MOSI  = geom_dbus.dat_w,
            o_dBusWishbone_SEL       = geom_dbus.sel,
            o_dBusWishbone_CYC       = geom_dbus.cyc,
            o_dBusWishbone_STB       = geom_dbus.stb,
            o_dBusWishbone_WE        = geom_dbus.we,
            o_dBusWishbone_CTI       = geom_dbus.cti,
            o_dBusWishbone_BTE       = geom_dbus.bte,
            i_dBusWishbone_DAT_MISO  = geom_dbus.dat_r,
            i_dBusWishbone_ACK       = geom_dbus.ack,
            i_dBusWishbone_ERR       = geom_dbus.err,
        )

        platform.add_source(
            os.path.join(os.path.dirname(__file__), "..", "rtl", "geom", "VexRiscvGeom.v")
        )

    # Audio core window (rtl/audio/AudioCore.v): IMEM, DMEM, CTRL. In the
    # game CPU's uncached I/O region (>= 0x8000_0000), so its mailbox and
    # status words are never read stale from the game CPU's D-cache.
    AUDIO_BASE = 0x8000_0000
    AUDIO_SIZE = 0x0000_8000

    def add_audio_subsystem(self, platform):
        # SoC -> audio core: the game CPU loads IMEM/DMEM here, sets RUN, and
        # talks to the core through the CTRL mailbox words.
        audio_slave = wishbone.Interface(data_width=32, address_width=32, addressing="word")
        self.bus.add_slave(name="audio", slave=audio_slave,
                           region=SoCRegion(origin=self.AUDIO_BASE, size=self.AUDIO_SIZE,
                                            mode="rw", cached=False))
        # Audio core -> SoC: everything outside its window (SDRAM, CSRs such
        # as the APF audio FIFO) leaves through this one master port.
        audio_master = wishbone.Interface(data_width=32, address_width=32, addressing="word")
        self.bus.add_master("audio_cpu", audio_master)

        self.specials += Instance(
            "AudioCore",
            i_clk     = ClockSignal("sys"),
            i_rst     = ResetSignal("sys"),
            i_s_adr   = audio_slave.adr,
            i_s_dat_w = audio_slave.dat_w,
            o_s_dat_r = audio_slave.dat_r,
            i_s_sel   = audio_slave.sel,
            i_s_cyc   = audio_slave.cyc,
            i_s_stb   = audio_slave.stb,
            i_s_we    = audio_slave.we,
            o_s_ack   = audio_slave.ack,
            o_m_adr   = audio_master.adr,
            o_m_dat_w = audio_master.dat_w,
            i_m_dat_r = audio_master.dat_r,
            o_m_sel   = audio_master.sel,
            o_m_cyc   = audio_master.cyc,
            o_m_stb   = audio_master.stb,
            o_m_we    = audio_master.we,
            i_m_ack   = audio_master.ack,
            i_m_err   = audio_master.err,
        )
        rtl = os.path.join(os.path.dirname(__file__), "..", "rtl", "audio")
        platform.add_source(os.path.join(rtl, "AudioCore.v"))
        platform.add_source(os.path.join(rtl, "AudioDspCfu.v"))
        platform.add_source(os.path.join(rtl, "VexRiscvAudio.v"))

    def add_uart(self, platform):
        baudrate = 2000000

        uart_pads = platform.request("serial", loose=True)
        uart_phy = None
        uart = None
        fifo_depth = 16
        uart_kwargs = {
            "tx_fifo_depth": fifo_depth,
            "rx_fifo_depth": fifo_depth,
        }

        # JTAG UART
        from litex.soc.cores.jtag import JTAGPHY

        self.cd_sys_jtag = ClockDomain()
        self.comb += self.cd_sys_jtag.clk.eq(ClockSignal("sys"))
        jtag_uart_phy = JTAGPHY(
            device=platform.device, clock_domain="sys_jtag", platform=platform
        )

        # JTAG UART only (2026-09-24): the dev-kit cartridge serial UART and
        # its multiplexer went to make room -- every upload and log here goes
        # over JTAG. The serial and use_jtag pads stay as idle ports.
        platform.request("use_jtag")
        self.comb += uart_pads.tx.eq(1)

        uart = UART(jtag_uart_phy, **uart_kwargs)

        self.submodules.jtag_uart_phy = jtag_uart_phy
        self.submodules.uart = uart

        # IRQ.
        if self.irq.enabled:
            self.irq.add("uart", use_loc_if_exists=True)
        else:
            self.add_constant("UART_POLLING")


# Build --------------------------------------------------------------------------------------------


def rewrite_output_variables(root_dir: str, generated_dir: str):
    filename = os.path.join(generated_dir, "variables.mak")

    print(f"Rewriting {filename} from {root_dir}")

    if os.path.exists(filename):
        from fileinput import FileInput

        with FileInput(filename, inplace=True, backup=".bak") as file:
            for line in file:
                print(line.replace(root_dir, "$(LITEX_ROOT_DIRECTORY)"), end="")
    else:
        print("Cannot find `variables.mak`")


def main():
    from litex.build.parser import LiteXArgumentParser
    import sys

    # LiteX directly reaches into sys.argv multiple times, so we have to inject all of our changed arguments at top level
    # Match up with Rust compiler target with FPU and RVC
    sys.argv.extend(
        [
            "--cpu-type=vexriscv_smp",
            "--with-fpu",
            "--with-rvc",
            # Bare-metal game CPU (2026-09-24): the SMP cluster's Linux
            # machinery -- MMU and TLBs, supervisor mode, LR/SC/AMO, the
            # hardware breakpoint -- is dead area here (no firmware uses it;
            # the game binary has no atomics and never reads `time`). The
            # vendored LiteX / VexRiscv carry the switches (local commits).
            # The arch becomes rv32imfc: Rust targets must drop `a` too.
            "--without-mmu",
            "--without-supervisor",
            "--without-atomic",
            "--hardware-breakpoints=0",
            "--without-debug",          # no JTAG CPU debug bridge (never used)
            # UART is manually added
            "--no-uart",
            "--timer-uptime",
            # Stock LiteX BIOS in ROM: enables `litex_term --jtag-config=...
            # --kernel <file>.bin jtag` to upload/run a program into SDRAM over
            # the JTAG UART, so firmware iteration no longer needs a Quartus
            # recompile. (Replaces the earlier `--integrated-rom-init=
            # rom_boot_hello + --no-compile-software` diagnostic checkpoint.)
            # ROM_BOOT_ADDRESS=0x40000000 is still set above, so the BIOS tries
            # romboot from empty SDRAM, fails, and falls through to serialboot.
            # 8 KiB: only the BIOS uses it (1.7 KiB .data/.bss + stack);
            # programs run from SDRAM. 32 KiB cost 24 M10Ks the audio core
            # needs.
            "--integrated-sram-size=0x2000",
            # The BIOS is ~20 KiB: a 128 KiB ROM held 32 M10Ks for it, a 22 KiB
            # one 22, and the ten go to the game CPU's caches below.
            "--integrated-rom-size=0x5800",
            # Game CPU caches (2026-09-26): 16 KiB 4-way I$ + 8 KiB 2-way D$
            # (were 4 KiB direct-mapped each; VexRiscv caps a way at 4 KiB).
            # Measured on sim/game_cpu (the real cluster netlist running
            # Bob-omb Battlefield): 35.5 -> 23.0 ms of game-CPU work a frame;
            # the CPU is memory-bound on the Pocket. +20 M10K, ~+90 ALM.
            "--icache-size=16384",
            "--icache-ways=4",
            "--dcache-size=8192",
            "--dcache-ways=2",
        ]
    )

    parser = LiteXArgumentParser(
        platform=analogue_pocket.Platform, description="LiteX SoC on Analog Pocket."
    )
    parser.add_target_argument(
        "--sys-clk-freq",
        default=CLOCK_SPEED,
        type=float,
        help="System clock frequency.",
    )
    args = parser.parse_args()

    soc_args = parser.soc_argdict

    soc = BaseSoC(sys_clk_freq=args.sys_clk_freq, **soc_args)
    builder_args = parser.builder_argdict
    builder_args["csr_svd"] = "pocket.svd"
    builder = Builder(soc, **builder_args)

    root_dir = os.path.abspath("")
    generated_dir = builder.generated_dir

    if args.build:
        builder.build(**parser.toolchain_argdict)

    if args.load:
        prog = soc.platform.create_programmer()
        prog.load_bitstream(
            builder.get_bitstream_filename(mode="sram").replace(".sof", ".rbf")
        )

    # Make `variables.mak` use relative paths off of `LITEX_ROOT_DIRECTORY`
    rewrite_output_variables(root_dir, generated_dir)


if __name__ == "__main__":
    main()
