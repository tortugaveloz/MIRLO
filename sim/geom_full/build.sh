#!/bin/bash
# Builds the geom-core RTL simulation (real VexRiscvGeom + Vpu4DFixed) into
# obj_dir/tb_geom_cpu, plus the host-side comparison tools into tools/.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"; R="$(cd "$HERE/../.." && pwd)"
export VERILATOR_ROOT="${VERILATOR_ROOT:-$HOME/local/usr/share/verilator}"
VERILATOR="${VERILATOR:-$HOME/local/usr/bin/verilator}"
# GeomSetupUnit options: default = what projects/openFPGA-RISC-V_pocket.qsf
# deploys (the full unit). SU_DEFINES="+define+SU_NO_RECIP +define+SU_NO_NORMF"
# builds the reduced one.
# GEOM_CPU_V=<netlist> / MDIR=<dir>: simulate another geom CPU build (e.g. a
# different I-cache size) next to the default one.
GEOM_CPU_V="${GEOM_CPU_V:-$R/rtl/geom/VexRiscvGeom.v}"
MDIR="${MDIR:-$HERE/obj_dir}"
CSR_H="$R/litex/build/litex/software/include/generated/csr.h"
csr_ofs() { sed -n "s/^#define $1 (CSR_BASE + \(0x[0-9a-fA-F]*\)L)/\1/p" "$CSR_H"; }
MB_OFS=$(csr_ofs CSR_MAILBOX_BASE)
# MRDP's command CSRs, and the MRDP C model linked into the bench (TB_MRDP)
MRDP_OFS=$(csr_ofs CSR_MRDP_BASE)
MRDP_DEF="-DTB_MRDP -DGEOM_MRDP"; MRDP_SRC="$R/lang/c/mrdp/mrdp.c"
# the gateware leaves the setup unit's edge-setup and blend ops out (the qsf's SU_NO_EDGE, SU_NO_BLEND)
SU_DEFINES="${SU_DEFINES-+define+SU_NO_EDGE +define+SU_NO_BLEND}"
[ -n "$MB_OFS" ] && [ -n "$MRDP_OFS" ] || { echo "csr.h: mailbox/mrdp base not found" >&2; exit 1; }
# Layout inside each block is checked too (tb assumes these exact offsets).
for chk in "CSR_MAILBOX_STATUS_ADDR 0x18" "CSR_MAILBOX_GEOM_MSG_ADDR 0xc" "CSR_MRDP_CMD_STATUS_ADDR 0x4" \
           "CSR_MRDP_SYNC_COUNT_ADDR 0xc" "CSR_MRDP_LOAD_COUNT_ADDR 0x10" "CSR_MRDP_STATUS_ADDR 0x14"; do
    set -- $chk; base=$([ "${1#CSR_MAILBOX}" != "$1" ] && echo $MB_OFS || echo $MRDP_OFS)
    [ $(( $(csr_ofs $1) - base )) -eq $(( $2 )) ] || { echo "csr.h: $1 moved inside its block" >&2; exit 1; }
done
echo "tb: mailbox @ CSR+$MB_OFS, MRDP @ CSR+$MRDP_OFS"
# IBUS_SIMPLE=1: a GEOM_IBUS=simple core (no I-cache, iBus from the ROM's port)
IBUS_DEF=""; IBUS_CF=""
# (default: whatever the netlist has)
[ -z "${IBUS_SIMPLE:-}" ] && { grep -q "iBus_cmd_valid" "$GEOM_CPU_V" && IBUS_SIMPLE=1 || IBUS_SIMPLE=0; }
[ "$IBUS_SIMPLE" = 1 ] && { IBUS_DEF="+define+GEOM_IBUS_SIMPLE"; IBUS_CF="-DGEOM_IBUS_SIMPLE"; }
# a GEOM_DTCM=1 core: the geom RAM on the core's tightly coupled data port
grep -q "dTcm_enable" "$GEOM_CPU_V" && { IBUS_DEF="$IBUS_DEF +define+GEOM_DTCM"; IBUS_CF="$IBUS_CF -DGEOM_DTCM"; }
"$VERILATOR" --cc -CFLAGS "-DTB_MB_OFS=${MB_OFS}u -DTB_MRDP_OFS=${MRDP_OFS}u $MRDP_DEF $IBUS_CF ${TB_CF:-}" $IBUS_DEF --exe --build -O2 -Wno-fatal -Wno-lint -Wno-style --public-flat-rw \
    --top-module geom_cpu_top $SU_DEFINES -I"$R/rtl/vpu" -Mdir "$MDIR" -o tb_geom_cpu -j 8 \
    "$HERE/geom_cpu_top.v" "$GEOM_CPU_V" "$R/rtl/vpu/Vpu4DFixed.v" "$R/rtl/vpu/GeomSetupUnit.v" \
    "$HERE/tb_geom_cpu.cpp" $MRDP_SRC
CF="-O2 -w -std=gnu11 -fno-strict-aliasing -m64 -fcommon -ffp-contract=off -fsigned-char \
    -DGEOM_HOST_TEST -DGEOM_FB_HRES=268 -DGEOM_FB_VRES=240 ${MRDP_DEF:+-DGEOM_MRDP}"
INC="-I$R/lang/c/geom -I$R/lang/c/game"
# shellcheck disable=SC2086
gcc $CF $INC -o "$HERE/tools/hostreplay" "$HERE/tools/hostreplay.c" \
    "$R/lang/c/geom/geom_pipeline.c" "$R/lang/c/geom/geom_triangle.c" \
    "$R/lang/c/geom/geom_fixed.c" "$R/lang/c/geom/geom_vecmath.c" $MRDP_SRC -lm
gcc -O1 -w -I"$R/lang/c/geom" -I"$R/lang/c/game" -o "$HERE/tools/mkgdl" "$HERE/tools/mkgdl.c" -lm
