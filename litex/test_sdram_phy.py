#!/usr/bin/env python3
"""Migen simulation of HalfRateGENSDRAMPocketPHY behind a real LiteDRAM core.

Drives writes then reads through a native crossbar port into a small
behavioural SDR SDRAM model (ACT/READ/WRITE, CL, DM) and checks every word
reads back. It exists to check changes to the PHY's latency: the model's own
read timing is an idealisation (the real board has a phase-shifted SDRAM
clock), so the useful comparison is RELATIVE -- the committed PHY and the
working-tree PHY must both pass against the same model:

    python3 test_sdram_phy.py            # working-tree replaced_components.py
    python3 test_sdram_phy.py --rev HEAD # the PHY as committed at a git revision
"""
import argparse
import os
import random
import subprocess
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vendor  # noqa: F401,E402

from migen import *  # noqa: E402
from migen.fhdl.specials import Special  # noqa: E402
from migen.sim import run_simulation, passive  # noqa: E402

from litedram.core import LiteDRAMCore  # noqa: E402


class SimTristate(Special):
    """Stand-in for Tristate: exposes o/oe to the model and lets it drive i."""
    all = []

    def __init__(self, target, o, oe, i=None):
        Special.__init__(self)
        self.o, self.oe, self.i = o, oe, i
        self.drive = Signal(len(o))
        SimTristate.all.append(self)

    @staticmethod
    def lower(t):
        m = Module()
        m.comb += t.i.eq(t.drive)
        return m


def load_phy_module(rev):
    if rev is None:
        src = open(os.path.join(HERE, "replaced_components.py")).read()
    else:
        src = subprocess.check_output(["git", "show", f"{rev}:litex/replaced_components.py"], cwd=HERE).decode()
    mod = types.ModuleType("rc_under_test")
    exec(compile(src, "replaced_components.py", "exec"), mod.__dict__)
    mod.Tristate = SimTristate
    return mod


class Pads:
    def __init__(self):
        self.a = Signal(13)
        self.ba = Signal(2)
        self.ras_n = Signal(reset=1)
        self.cas_n = Signal(reset=1)
        self.we_n = Signal(reset=1)
        self.cke = Signal()
        self.dq = Signal(16)
        self.dm = Signal(2)


class DUT(Module):
    def __init__(self, rc, sys_clk_freq):
        self.pads = Pads()
        self.submodules.phy = rc.HalfRateGENSDRAMPocketPHY(self.pads, sys_clk_freq)
        module = rc.AS4C32M16Pocket(sys_clk_freq, "1:2")
        self.submodules.core = LiteDRAMCore(self.phy, module.geom_settings, module.timing_settings,
                                            sys_clk_freq)
        self.port = self.core.crossbar.get_port()


def chip_model(dut, cl, offset, log):
    pads = dut.pads
    rows = [0] * 4
    mem = {}
    pending = {}  # cycle -> 16-bit read data to drive
    wburst = None  # (key of the 2nd beat) while a BL=2 write is in progress
    lanes = SimTristate.all
    cyc = 0

    @passive
    def gen():
        nonlocal cyc, wburst
        while True:
            ras, cas, we = (yield pads.ras_n), (yield pads.cas_n), (yield pads.we_n)
            ba, a = (yield pads.ba), (yield pads.a)
            dm = yield pads.dm
            dq = 0
            oe = 1
            for i, t in enumerate(lanes):
                dq |= (yield t.o) << i
                oe &= (yield t.oe)
            def store(key):
                old = mem.get(key, 0)
                new = 0
                for b in range(2):
                    new |= (old if (dm >> b) & 1 else dq) & (0xff << (8 * b))
                mem[key] = new
            if wburst is not None:
                store(wburst)
                wburst = None
            if (ras, cas, we) == (0, 1, 1):
                rows[ba] = a
            elif (ras, cas, we) == (1, 0, 0):
                key = (ba, rows[ba], a & 0x3ff)
                wburst = (ba, rows[ba], (a & 0x3ff) ^ 1)
                if not oe:
                    log.append(f"WRITE without dq output enable at cycle {cyc}")
                store(key)
            elif (ras, cas, we) == (1, 0, 1):
                for k in range(2):  # BL=2, sequential within the aligned pair
                    pending[cyc + cl + offset + k] = mem.get((ba, rows[ba], (a & 0x3ff) ^ k), 0xdead)
            v = pending.pop(cyc + 1, 0)
            for i, t in enumerate(lanes):
                yield t.drive.eq((v >> i) & 1)
            cyc += 1
            yield
    return gen()


def host(dut, words, errors, done):
    port = dut.port
    for _ in range(200):
        yield

    def write(addr, data):
        yield port.cmd.valid.eq(1)
        yield port.cmd.we.eq(1)
        yield port.cmd.addr.eq(addr)
        yield
        while not (yield port.cmd.ready):
            yield
        yield port.cmd.valid.eq(0)
        yield port.wdata.valid.eq(1)
        yield port.wdata.data.eq(data)
        yield port.wdata.we.eq(0xf)
        yield
        while not (yield port.wdata.ready):
            yield
        yield port.wdata.valid.eq(0)

    def read(addr):
        yield port.cmd.valid.eq(1)
        yield port.cmd.we.eq(0)
        yield port.cmd.addr.eq(addr)
        yield port.rdata.ready.eq(1)
        yield
        while not (yield port.cmd.ready):
            yield
        yield port.cmd.valid.eq(0)
        n = 0
        while not (yield port.rdata.valid):
            yield
            n += 1
            if n > 200:
                return None
        v = yield port.rdata.data
        yield
        return v

    # Writes, then reads, then an interleaved write/read mix (RTW/WTR turnarounds).
    for addr, data in words:
        yield from write(addr, data)
    for addr, data in words:
        got = yield from read(addr)
        if got != data:
            errors.append(f"read {addr:#x}: got {got if got is None else hex(got)} want {data:#010x}")
    for addr, data in words[:32]:
        data2 = data ^ 0x5a5a5a5a
        yield from write(addr, data2)
        got = yield from read(addr)
        if got != data2:
            errors.append(f"rmw {addr:#x}: got {got if got is None else hex(got)} want {data2:#010x}")
    done.append(True)


def run(rev, offset):
    SimTristate.all = []
    rc = load_phy_module(rev)
    sys_clk_freq = 62.832e6
    dut = DUT(rc, sys_clk_freq)
    rnd = random.Random(1)
    addrs = rnd.sample(range(1 << 20), 96)  # spread over banks and rows
    words = [(a, rnd.getrandbits(32)) for a in addrs]
    errors, log, done = [], [], []
    run_simulation(dut, {
        "sys": [host(dut, words, errors, done)],
        "sys2x": [chip_model(dut, 3, offset, log)],
    }, clocks={"sys": 20, "sys2x": (10, 5)})  # rising edges coincide, as on the PLL
    return dut.phy.settings.read_latency, done, errors, log


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--rev", default=None)
    ap.add_argument("--offset", type=int, default=None,
                    help="model read-data offset in sys2x cycles (default: scan -2..2)")
    args = ap.parse_args()
    offsets = [args.offset] if args.offset is not None else range(-2, 3)
    for off in offsets:
        rl, done, errors, log = run(args.rev, off)
        status = "PASS" if done and not errors and not log else "FAIL"
        print(f"rev={args.rev or 'worktree'} read_latency={rl} model_offset={off:+d}: {status}"
              f" ({len(errors)} errors{', ' + errors[0] if errors else ''}{'; ' + log[0] if log else ''})")
