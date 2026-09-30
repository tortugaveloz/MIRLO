#!/bin/sh
# Prepares the vendored LiteX tree after a clone:
#   1. checks out the submodules at their upstream commits;
#   2. applies Mirlo's changes on top (patches/: the BIOS game loader, the
#      JTAG-UART upload fixes, the VexRiscv-SMP options, the scan-out DMA's
#      base latch, and the VexRiscv generators of the geometry and audio
#      cores);
#   3. puts the main CPU's generated netlist where LiteX looks for it, so the
#      build does not need Scala/sbt to regenerate it.
# Safe to run again: a patch that is already applied is skipped.
set -e
cd "$(dirname "$0")/../.."
git submodule update --init --recursive

apply() {   # apply <dir> <patch>
    if git -C "$1" apply --reverse --check "$2" 2>/dev/null; then
        echo "already applied: $2"
    else
        git -C "$1" apply "$2"
        echo "applied: $2"
    fi
}
P="$PWD/litex/vendor/patches"
apply litex/vendor/litex "$P/litex.patch"
apply litex/vendor/litedram "$P/litedram.patch"
apply litex/vendor/migen "$P/migen.patch"
apply litex/vendor/pythondata-cpu-vexriscv_smp/pythondata_cpu_vexriscv_smp/verilog/ext/VexRiscv "$P/VexRiscv.patch"

N=VexRiscvLitexSmpCluster_Cc1_Iw32Is16384Iy4_Dw32Ds8192Dy2_ITs4DTs4_Ldw32_Ood_Fpu4_Rvc_Nmmu_Nsv_Na_Ndbg.v
cp rtl/litex/$N litex/vendor/pythondata-cpu-vexriscv_smp/pythondata_cpu_vexriscv_smp/verilog/$N
echo "netlist: $N"
