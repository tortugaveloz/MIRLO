#!/bin/bash
# Builds the AudioDspCfu unit bench into obj_cfu/tb_cfu.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"; R="$(cd "$HERE/../.." && pwd)"
export VERILATOR_ROOT="${VERILATOR_ROOT:-$HOME/local/usr/share/verilator}"
VERILATOR="${VERILATOR:-$HOME/local/usr/bin/verilator}"
mkdir -p "$HERE/obj_cfu"
gcc -O2 -c -I"$R/lang/c/audio" "$R/lang/c/audio/audio_cfu_table.c" -o "$HERE/obj_cfu/table_host.o"   # the model's table
"$VERILATOR" --cc --exe --build -O2 -Wno-fatal -Wno-lint -Wno-style ${CFU_DEFINES:-} \
    --top-module AudioDspCfu -Mdir "$HERE/obj_cfu" -o tb_cfu -j 8 \
    -CFLAGS "-I$R/lang/c/audio" \
    -LDFLAGS "$HERE/obj_cfu/table_host.o" \
    "$R/rtl/audio/AudioDspCfu.v" "$HERE/tb_cfu.cpp"
