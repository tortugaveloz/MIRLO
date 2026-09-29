"""
Parallel fixed-point matrix-vector engine -- attached to the geometry core as
a VexRiscv CFU (custom function unit); docs/geometry_core.md. Geometry core
only: the game CPU has its own scalar FPU and no path to this unit.

Not a CSR peripheral -- there is nothing on the bus for it. The geometry
firmware talks to it purely through custom `custom-0` instructions:

  function id = {funct3[2:0], funct7[6:0]}   (10-bit), rs1 -> inputs_0, rs2 -> inputs_1
  (SpinalHDL Cat() order -- see lang/c/geom/geom_vpar.h's GEOM_VPAR_INSN)
    0x00  M[rs2&0xF] = rs1   (resident 4x4 matrix element, row-major, S17.10)
    0x01  A[rs2&0x3] = rs1   (resident input vector element, S17.10)
    0x30  MATVEC4            out[j] = sum_i A[i]*M[i*4+j] for all 4 j at once
    0x08  rd = out[rs2&0x3]  read one output component

The CFU-L1 bus shape (8 signals); rtl/vpu/Vpu4DFixed.v's header explains
the four real multiply-accumulate lanes behind MATVEC4.
"""

import os

from migen import *

from litex.gen.fhdl.module import LiteXModule

VPU_ROOT = os.path.join(os.path.dirname(__file__), "..", "rtl", "vpu")


class VPUFixedCfu(LiteXModule):
    def __init__(self, platform, data_width=27, frac_bits=10):
        # CFU-L1 bus. Driven by the geometry core; consumed by Vpu4DFixed.
        self.cmd_valid       = Signal()
        self.cmd_ready       = Signal()
        self.cmd_function_id = Signal(10)
        self.cmd_inputs_0    = Signal(32)
        self.cmd_inputs_1    = Signal(32)
        self.rsp_valid       = Signal()
        self.rsp_ready       = Signal()
        self.rsp_outputs_0   = Signal(32)
        # GeomSetupUnit's PUSH (0x61): triangle packets into MRDP's command
        # FIFO (MRDPCore.su_*), not through the core's CSR stores
        self.out_valid       = Signal()
        self.out_ready       = Signal()
        self.out_data        = Signal(32)

        self.specials += Instance("Vpu4DFixed",
            p_DATA_WIDTH      = data_width,
            p_FRAC_BITS       = frac_bits,
            i_clk             = ClockSignal("sys"),
            i_resetn          = ~ResetSignal("sys"),
            i_cmd_valid       = self.cmd_valid,
            o_cmd_ready       = self.cmd_ready,
            i_cmd_function_id = self.cmd_function_id,
            i_cmd_inputs_0    = self.cmd_inputs_0,
            i_cmd_inputs_1    = self.cmd_inputs_1,
            o_rsp_valid       = self.rsp_valid,
            i_rsp_ready       = self.rsp_ready,
            o_rsp_outputs_0   = self.rsp_outputs_0,
            o_out_valid       = self.out_valid,
            i_out_ready       = self.out_ready,
            o_out_data        = self.out_data,
        )

        platform.add_source(os.path.join(VPU_ROOT, "Vpu4DFixed.v"))
        platform.add_source(os.path.join(VPU_ROOT, "GeomSetupUnit.v"))
        platform.add_verilog_include_path(VPU_ROOT)
