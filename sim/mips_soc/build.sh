#!/bin/bash
# Verilator benches for MIRLO's MIPS SoC (rtl/soc/mirlo_mips.sv):
#   ./build.sh sdr   -> obj_sdr/tb_sdr   (SDRAM controller + PHY + arbiter vs a chip model)
#   ./build.sh soc   -> obj_soc/tb_soc   (the whole SoC; needs tools/mips_inits.py's images)
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"; R="$(cd "$HERE/../.." && pwd)"
export VERILATOR_ROOT="${VERILATOR_ROOT:-$HOME/local/usr/share/verilator}"
VL="${VERILATOR:-$HOME/local/usr/bin/verilator}"
case "${1:-soc}" in
sdr)
    "$VL" --cc --exe --build -j 8 -O2 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC --public-flat-rw -Mdir "$HERE/obj_sdr" \
        --top-module sdr_test_top "$R/rtl/soc/sdr_phy.sv" "$R/rtl/soc/sdr_ctrl.sv" "$R/rtl/soc/sdr_arb.sv" \
        "$HERE/sdr_test_top.sv" "$HERE/tb_sdr.cpp" -o tb_sdr > "$HERE/obj_sdr.log" 2>&1 || { tail -30 "$HERE/obj_sdr.log"; exit 1; }
    echo "OK -> $HERE/obj_sdr/tb_sdr" ;;
soc)
    # the Quartus project's macros (projects/mirlo.qsf, MIRLO_MIPS)
    DEFS="+define+SU_NO_EDGE=1 +define+SU_NO_BLEND=1 +define+MRDP_NO_2CYCLE=1 +define+MRDP_NO_TEXRECT=1 +define+MRDP_NO_PRIMENV=1 ${EXTRA_DEFS:-}"
    S="$R/rtl/soc"
    "$VL" --cc --exe --build -j 8 -O3 ${VL_THREADS:+--threads $VL_THREADS} -Wno-fatal -Wno-lint -Wno-style -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-MULTIDRIVEN \
        --x-assign fast --x-initial fast -Mdir "${OBJ:-$HERE/obj_soc}" --top-module soc_sim_top $DEFS -I"$S" -I"$R/rtl/vpu" \
        "$S/sdr_phy.sv" "$S/sdr_ctrl.sv" "$S/sdr_arb.sv" "$S/afifo.sv" "$S/fifo_fwft.sv" "$S/n64_video.sv" \
        "$S/n64_bus.sv" "$S/mirlo_regs.sv" "$S/n64_ports.sv" "$S/n64_geom.sv" "$S/n64_uart.sv" "$S/jtag_uart_bridge.v" \
        "$HERE/sim_cyclonev_jtag.v" "$S/mips_sys.sv" "$S/mirlo_mips.sv" \
        "$R/rtl/mips/fpu_n64.sv" "$R/rtl/mips/mips_core.sv" "$R/rtl/mips/mips_lite.sv" "$R/rtl/mips/mips_geom.sv" \
        "$HERE/sim_GeomTcmRam.v" "$R/rtl/vpu/Vpu4DFixed.v" "$R/rtl/vpu/GeomSetupUnit.v" \
        "$R/rtl/audio/AudioCore.v" "$R/rtl/audio/AudioDspCfu.v" \
        "$R"/rtl/mrdp/mrdp_*.v \
        "$HERE/soc_sim_top.sv" "$HERE/tb_soc.cpp" -o tb_soc > "${OBJ:-$HERE/obj_soc}.log" 2>&1 || { grep -E "%Error" "${OBJ:-$HERE/obj_soc}.log" | head -30; tail -5 "${OBJ:-$HERE/obj_soc}.log"; exit 1; }
    echo "OK -> ${OBJ:-$HERE/obj_soc}/tb_soc" ;;
esac
