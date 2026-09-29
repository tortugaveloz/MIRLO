# MRDP -- Mirlo's N64-style rasterizer (docs/mrdp.md, rtl/mrdp/). The RTL has
# its own LiteDRAM native port; commands arrive through a
# FIFO with two producers: CSR writes to `cmd_data` (the game CPU and the geom
# core over the bus) and the geom CFU's PUSH port (su_*).
from migen import *

from litex.gen import LiteXModule
from litex.soc.interconnect import stream
from litex.soc.interconnect.csr import CSR, CSRField, CSRStatus

_MRDP_SOURCES = ["mrdp_top.v", "mrdp_pix.v", "mrdp_mem.v", "mrdp_sdpram.v", "mrdp_rcp_rom.v", "mrdp_seed_rom.v", "mrdp_ucode_rom.v"]


class MRDPCore(LiteXModule):
    def __init__(self, platform, dram_port, dram_base=0x40000000, cmd_fifo_depth=2048):
        # ---- command FIFO: CSR writes win a same-cycle clash with the CFU
        self.cmd_fifo = stream.SyncFIFO([("data", 32)], depth=cmd_fifo_depth)
        self.cmd_data = CSR(32)
        self.cmd_data.description = ("Push one command word. Poll cmd_status.level first: "
                                     "a write to a full FIFO is lost (and counted in cmd_dropped).")
        self.su_valid = Signal()
        self.su_ready = Signal()
        self.su_data  = Signal(32)
        self.cmd_status = CSRStatus(fields=[
            CSRField("level", size=len(self.cmd_fifo.level), description="Command FIFO fill level."),
            CSRField("full",  size=1, description="Command FIFO full: a write now would be lost."),
        ])
        csr_we = self.cmd_data.re
        self.comb += [
            self.cmd_fifo.sink.valid.eq(csr_we | self.su_valid),
            self.cmd_fifo.sink.data.eq(Mux(csr_we, self.cmd_data.r, self.su_data)),
            self.su_ready.eq(~csr_we & self.cmd_fifo.sink.ready),
            self.cmd_status.fields.level.eq(self.cmd_fifo.level),
            self.cmd_status.fields.full.eq(~self.cmd_fifo.sink.ready),
        ]
        cmd_dropped = Signal(16)
        self.sync += If(csr_we & ~self.cmd_fifo.sink.ready,
            If(cmd_dropped != 2**16 - 1, cmd_dropped.eq(cmd_dropped + 1)))
        self.cmd_dropped = CSRStatus(16, description="Command words written while the FIFO was full (lost).")
        self.comb += self.cmd_dropped.status.eq(cmd_dropped)

        # ---- status
        self.sync_count = CSRStatus(32, description="SYNC FULLs completed: every write before them is in DRAM.")
        self.load_count = CSRStatus(32, description="LOAD TILEs completed (their staging memory is free again).")
        self.status = CSRStatus(fields=[
            CSRField("idle", size=1, description="No command in progress, nothing queued, no write pending."),
            CSRField("unknown_ops", size=16, offset=16, description="Unknown opcodes skipped."),
        ])
        idle = Signal()
        unknown = Signal(16)
        self.comb += [
            self.status.fields.idle.eq(idle & (self.cmd_fifo.level == 0) & ~self.cmd_fifo.source.valid),
            self.status.fields.unknown_ops.eq(unknown),
        ]

        # ---- the core and its memory port
        addr = Signal(32)
        self.comb += [
            dram_port.cmd.addr.eq((addr - dram_base)[2:]),
            dram_port.cmd.last.eq(1),
            dram_port.rdata.ready.eq(1),
        ]
        self.specials += Instance("mrdp_top",
            i_clk=ClockSignal("sys"), i_rst=ResetSignal("sys"),
            i_cmd_valid=self.cmd_fifo.source.valid, o_cmd_ready=self.cmd_fifo.source.ready,
            i_cmd_data=self.cmd_fifo.source.data,
            o_m_cmd_valid=dram_port.cmd.valid, i_m_cmd_ready=dram_port.cmd.ready,
            o_m_cmd_we=dram_port.cmd.we, o_m_cmd_addr=addr,
            o_m_wdata_valid=dram_port.wdata.valid, i_m_wdata_ready=dram_port.wdata.ready,
            o_m_wdata=dram_port.wdata.data, o_m_wdata_we=dram_port.wdata.we,
            i_m_rdata_valid=dram_port.rdata.valid, i_m_rdata=dram_port.rdata.data,
            o_sync_count=self.sync_count.status, o_load_count=self.load_count.status,
            o_unknown_ops=unknown, o_idle=idle,
        )
        import os
        rtl = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "rtl", "mrdp")
        for f in _MRDP_SOURCES:
            platform.add_source(os.path.join(rtl, f))
