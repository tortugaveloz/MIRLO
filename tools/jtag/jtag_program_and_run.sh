#!/bin/bash
# Program the FPGA over the USB-Blaster, then upload and run one firmware
# image over the JTAG UART and log its output -- no Pocket menu, no
# controller. Reprogramming doubles as the core reset, so this can be run
# back to back for a battery of phase harnesses.
#
#   ./jtag_program_and_run.sh <bitstream.sof> <firmware.bin> <log> [seconds] [until-text]
#
# Needs target/pocket/core_top.sv's use_jtag to reset to 1 (it does since
# 2026-09-23): a bitstream loaded this way never receives the APF's write of
# the "Enable JTAG UART" setting.
set -u
SOF="$1"; BIN="$2"; LOG="$3"; SECS="${4:-120}"; UNTIL="${5:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"
# Absolute path, NOT a PATH prepend: Quartus ships its own openocd without the
# interface/*.cfg files openocd_rpc.cfg needs, and it would shadow the system one.
QUARTUS_PGM="${QUARTUS_PGM:-$HOME/altera/25.1std/quartus/bin/quartus_pgm}"
W="${TMPDIR:-/tmp}/jtag_par.$$"

# Free the Blaster. Match exact process names / anchored command lines so the
# pattern cannot match this shell's own command line.
for p in $(pgrep -x openocd) $(pgrep -f '^python3 -u jtag_uart_relay.py') $(pgrep -f '^python3 -u .*jtag_uart_relay.py'); do
    kill "$p" 2>/dev/null
done
sleep 1

echo "[1/4] quartus_pgm $SOF"
"$QUARTUS_PGM" -c "USB-Blaster [1-5]" -m JTAG -o "p;$SOF" > "$W.pgm" 2>&1
grep -q "Configuration succeeded" "$W.pgm" || { echo "  programming failed:"; tail -5 "$W.pgm"; exit 1; }

cd "$HERE"
echo "[2/4] openocd"
openocd -f openocd_rpc.cfg > "$W.oo" 2>&1 &
OO=$!
for _ in $(seq 1 40); do grep -q 'tap/device found' "$W.oo" && break; sleep 0.5; done
grep -q 'tap/device found' "$W.oo" || { echo "  TAP not found"; tail -5 "$W.oo"; kill $OO; exit 1; }

echo "[3/4] jtag_uart_relay.py"
python3 -u jtag_uart_relay.py > "$W.relay" 2>&1 &
RL=$!
PTS=""
for _ in $(seq 1 40); do PTS="$(sed -n 1p "$W.relay" 2>/dev/null || true)"; [ -n "$PTS" ] && [ -e "$PTS" ] && break; sleep 0.25; done
[ -e "$PTS" ] || { echo "  relay pty not ready"; cat "$W.relay"; kill $OO $RL; exit 1; }

echo "[4/4] jtag_run.py $BIN -> $LOG"
ARGS=(--seconds "$SECS" --log "$LOG")
[ -n "$UNTIL" ] && ARGS+=(--until "$UNTIL")
python3 jtag_run.py "$PTS" "$BIN" "${ARGS[@]}"
rc=$?
kill $RL $OO 2>/dev/null
exit $rc
