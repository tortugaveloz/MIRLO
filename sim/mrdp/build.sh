#!/bin/bash
# Verilate the MRDP RTL against the C model. Needs VERILATOR_ROOT (where
# Verilator's include/ and bin/ are).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
R="$HERE/../../rtl/mrdp"
export VERILATOR_ROOT="${VERILATOR_ROOT:-$HOME/local/usr/share/verilator}"
V="${VERILATOR:-$HOME/local/usr/bin/verilator}"
cd "$HERE"
"$V" --cc --exe --build -j 8 -O3 --x-assign fast --x-initial fast \
    -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-BLKSEQ -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
    --top-module mrdp_top ${VDEFS:-} \
    "$R/mrdp_top.v" "$R/mrdp_pix.v" "$R/mrdp_mem.v" "$R/mrdp_sdpram.v" "$R/mrdp_rcp_rom.v" "$R/mrdp_seed_rom.v" "$R/mrdp_ucode_rom.v" \
    tb_mrdp.cpp "$HERE/../../lang/c/mrdp/mrdp.c" \
    -CFLAGS "-O2 -I$HERE ${TB_CFLAGS:-}" -o tb_mrdp > build.log 2>&1 || { tail -30 build.log; exit 1; }
echo "OK -> $HERE/obj_dir/tb_mrdp"
