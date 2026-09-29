#!/bin/bash
# Builds the AudioCore Verilator bench into obj_dir/tb_audio.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"; R="$(cd "$HERE/../.." && pwd)"
export VERILATOR_ROOT="${VERILATOR_ROOT:-$HOME/local/usr/share/verilator}"
VERILATOR="${VERILATOR:-$HOME/local/usr/bin/verilator}"
"$VERILATOR" --cc --exe --build -O2 -Wno-fatal -Wno-lint -Wno-style \
    --top-module AudioCore -Mdir "$HERE/obj_dir" -o tb_audio -j 8 \
    "$R/rtl/audio/AudioCore.v" "$R/rtl/audio/AudioDspCfu.v" "$R/rtl/audio/VexRiscvAudio.v" "$HERE/tb_audio.cpp"
