#!/usr/bin/env python3

"""
MRDP (litex/mrdp.py) inside a cut-down SoC, under Verilator: the game-CPU
family (VexRiscv-SMP) writing MRDP's command FIFO over the real CSR bus, and
MRDP on its own port of the real LiteDRAM controller + crossbar, backed by a
behavioural SDRAM model with the Pocket's module timings.

sim/mrdp proves the RTL equal to the C model against a hand-written memory
model; this covers what that cannot: the CSR/FIFO wiring, the native port's
address translation and handshakes, and a CPU sharing the memory.

    cd sim/soc_firmware && ./mkcap_mrdp.sh && make SIM_SOC=sim_mrdp FW_MAIN=mrdp_main.o OBJECTS="init_asm.o mrdp_main.o"
    cd litex && python3 sim_mrdp.py --firmware ../sim/soc_firmware/build/build.bin
    build/sim_mrdp/gateware/obj_dir/Vsim     (prints PASS / FAIL)

Needs the LiteX environment (vendor/) and Verilator (VERILATOR_ROOT set).
"""

import argparse
import os

import vendor  # noqa: F401  (sets up import paths for litex/migen)

from migen import *

from litex.build.generic_platform import Pins, Subsignal
from litex.build.sim import SimPlatform
from litex.build.sim.config import SimConfig

from litex.build.io import CRG
from litex.soc.integration.common import get_mem_data
from litex.soc.integration.builder import Builder, builder_args, builder_argdict
from litex.soc.integration.soc_core import SoCCore, soc_core_args, soc_core_argdict
from litex.soc.cores.cpu.vexriscv_smp import VexRiscvSMP

from mrdp import MRDPCore

# The Pocket maps SDRAM here (see analogue_pocket.py); keeping the same base
# means firmware pointers are identical between simulation and hardware.
MAIN_RAM_BASE = 0x40000000

_io = [
    ("sys_clk", 0, Pins(1)),
    ("sys_rst", 0, Pins(1)),
    ("serial", 0,
        Subsignal("source_valid", Pins(1)),
        Subsignal("source_ready", Pins(1)),
        Subsignal("source_data",  Pins(8)),
        Subsignal("sink_valid",   Pins(1)),
        Subsignal("sink_ready",   Pins(1)),
        Subsignal("sink_data",    Pins(8)),
    ),
]


class Platform(SimPlatform):
    def __init__(self):
        SimPlatform.__init__(self, "SIM", _io)


class MRDPSimSoC(SoCCore):
    def __init__(self, firmware=None, **kwargs):
        platform = Platform()
        sys_clk_freq = int(1e6)

        self.crg = CRG(platform.request("sys_clk"))

        # Same CPU family/ABI as the Pocket build, so the firmware that runs
        # here is built by the same toolchain against the same ISA.
        VexRiscvSMP.privileged_debug = False
        VexRiscvSMP.with_rvc = True
        # There is no LiteDRAM here to attach the CPU's direct memory bus to,
        # so route it over Wishbone to the integrated main RAM instead.
        VexRiscvSMP.wishbone_memory = True
        # The hardware build has the FPU on. It is off here because VexRiscv
        # asserts rfDataWidth <= memDataWidth and the FPU register file is
        # 64-bit, which a 32-bit Wishbone memory bus cannot satisfy (on the
        # Pocket the CPU reaches SDRAM over a 64-bit LiteDRAM native port
        # instead, so the constraint does not bite there). Nothing under test
        # here uses floating point: the firmware only writes CSRs.
        VexRiscvSMP.with_fpu = False

        SoCCore.__init__(self, platform,
            clk_freq              = sys_clk_freq,
            ident                 = "MRDP SoC simulation",
            cpu_type              = "vexriscv_smp",
            cpu_variant           = "linux",
            # The sim platform's serial pads are the stream-style pads the
            # serial2console sim module expects, not RS232 tx/rx.
            uart_name             = "sim",
            # With a firmware supplied it replaces the BIOS in ROM entirely
            # and the CPU resets straight into it (VexRiscvSMP asserts its
            # reset address is 0, so the firmware has to be ROM-resident).
            #
            # Going through the BIOS is not an option: this project patches
            # boot.c to pull images over the Pocket's APF bridge, whose CSRs
            # this cut-down SoC does not have. Driving the BIOS console
            # interactively is not reliable either -- the simulated UART at
            # 1 MHz drops characters when a whole line is written at once.
            integrated_rom_size   = 0x10000,
            integrated_rom_init   = (
                get_mem_data(firmware, endianness="little") if firmware else []),
            integrated_sram_size  = 0x2000,
            integrated_main_ram_size = 0,
            **kwargs)

        from litedram.phy.model import SDRAMPHYModel
        from replaced_components import AS4C32M16Pocket
        sdram_module = AS4C32M16Pocket(int(100e6), "1:1")
        self.sdrphy = SDRAMPHYModel(module=sdram_module, data_width=32, clk_freq=int(100e6))
        self.add_sdram("sdram", phy=self.sdrphy, module=sdram_module, l2_cache_size=0)

        # The framebuffer region the firmware renders into. On the Pocket this
        # is carved out of SDRAM by add_video_framebuffer(); here it is just a
        # window into main RAM at the same offset.
        self.add_constant("VIDEO_FRAMEBUFFER_BASE", MAIN_RAM_BASE + 0x00100000)
        self.add_constant("VIDEO_FRAMEBUFFER_HRES", 64)
        self.add_constant("VIDEO_FRAMEBUFFER_VRES", 64)
        # Skip the BIOS main-RAM memtest: at the 1 MHz simulated clock walking
        # 2 MiB takes many minutes of wall time, and the RAM model is not what
        # is being tested here. When a firmware is preloaded LiteX already
        # defines this itself (a pre-initialised RAM must not be memtested).
        if not firmware:
            self.add_config("MAIN_RAM_INIT")

        # This project's BIOS main.c references these (they come from the
        # Pocket's video timings on hardware); define them so the same BIOS
        # source builds for the simulation target.
        self.add_constant("MAX_DISPLAY_WIDTH", 64)
        self.add_constant("MAX_DISPLAY_HEIGHT", 64)

        # The thing under test -- exactly as the bitstream instantiates it.
        self.mrdp = MRDPCore(platform, dram_port=self.sdram.crossbar.get_port(),
                             dram_base=MAIN_RAM_BASE)


def main():
    parser = argparse.ArgumentParser(description="MRDP SoC simulation")
    parser.add_argument("--trace", action="store_true", help="Dump a VCD trace.")
    parser.add_argument("--trace-start", default=0, type=int)
    parser.add_argument("--trace-end", default=-1, type=int)
    parser.add_argument("--threads", default=4, type=int)
    parser.add_argument("--firmware", default=None, help="Bare-metal .bin to boot from ROM.")
    parser.add_argument("--run", action="store_true", help="Run the simulation after building.")
    builder_args(parser)
    soc_core_args(parser)
    args = parser.parse_args()

    sim_config = SimConfig()
    sim_config.add_clocker("sys_clk", freq_hz=int(1e6))
    sim_config.add_module("serial2console", "serial")

    soc_kwargs = soc_core_argdict(args)
    for k in ("integrated_rom_size", "integrated_sram_size",
              "integrated_main_ram_size", "cpu_type", "cpu_variant",
              "uart_name"):
        soc_kwargs.pop(k, None)

    soc_kwargs["timer_uptime"] = True     # the firmware times itself
    soc = MRDPSimSoC(firmware=args.firmware, **soc_kwargs)

    builder_kwargs = builder_argdict(args)
    builder_kwargs["output_dir"] = builder_kwargs.get("output_dir") or "build/sim_mrdp"
    if args.firmware:
        builder_kwargs["compile_software"] = False
    builder = Builder(soc, **builder_kwargs)
    builder.build(
        sim_config  = sim_config,
        trace       = args.trace,
        trace_start = args.trace_start,
        trace_end   = args.trace_end,
        threads     = args.threads,
        run         = args.run,
        opt_level   = "O3",
        interactive = False,
    )


if __name__ == "__main__":
    main()
