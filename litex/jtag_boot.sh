#!/bin/bash
# Upload a bare-metal .bin to the Pocket's LiteX RISC-V core over JTAG (USB Blaster)
# and drop into its console. Requires the "BIOS-in-ROM" bitstream (integrated ROM
# holds the stock LiteX BIOS + SFL serialboot) and "Enable JTAG UART" ON in the
# in-core menu (Core Settings).
#
#   ./jtag_boot.sh path/to/build.bin
#
# Then, on the Pocket:  in-core menu (select + D-pad-down) -> Core Settings ->
# "Reset core" -> press A (a few times if the first doesn't take). The BIOS
# reboots, hits "Booting from serial...", and this script's litex_term answers
# the SFL magic and uploads.
#
# Why the custom relay (jtag_uart_relay.py) instead of litex_term's own
# --jtag-config: the LiteX JTAGPHY XFER FSM resets on every Capture-DR
# (fsm.reset.eq(jtag.reset | jtag.capture)), which re-clears the `ready` bit
# before the host can read it -> host->target bytes are silently dropped, upload
# stalls at frame 0 forever. The relay keeps the TAP parked in Pause-DR between
# scans (openocd `drscan -endstate DRPAUSE`), so Pause-DR -> Exit2-DR -> Shift-DR
# never touches Capture-DR and the FSM (and flow-control `ready`) persist.
set -eu

BIN="${1:?usage: jtag_boot.sh path/to/build.bin [kernel_adr]}"
KERNEL_ADR="${2:-0x40000000}"   # where SFL uploads the .bin and then jumps
HERE="$(cd "$(dirname "$0")" && pwd)"
LOG="${TMPDIR:-/tmp}/jtag_boot.$$"

command -v openocd >/dev/null || { echo "openocd not found"; exit 1; }
[ -e "$BIN" ] || { echo "no such file: $BIN"; exit 1; }

cleanup() { kill $OO_PID $RELAY_PID 2>/dev/null || true; }
trap cleanup EXIT

echo "[1/3] openocd (TCL-RPC :6666)"
openocd -f "$HERE/openocd_rpc.cfg" > "$LOG.oo" 2>&1 &
OO_PID=$!
for _ in $(seq 1 40); do grep -q 'tap/device found' "$LOG.oo" && break; sleep 0.5; done
grep -q 'tap/device found' "$LOG.oo" || { echo "  TAP not found:"; tail -5 "$LOG.oo"; exit 1; }

echo "[2/3] jtag_uart_relay.py"
python3 -u "$HERE/jtag_uart_relay.py" > "$LOG.relay" 2>&1 &
RELAY_PID=$!
PTS=""
for _ in $(seq 1 40); do PTS="$(sed -n 1p "$LOG.relay" 2>/dev/null || true)"; [ -n "$PTS" ] && [ -e "$PTS" ] && break; sleep 0.25; done
[ -e "$PTS" ] || { echo "  relay pty not ready:"; cat "$LOG.relay"; exit 1; }
echo "  pty = $PTS"

case "$BIN" in
  *.json) MODE=(--images "$BIN") ;;                                   # multi-image manifest {file: addr}
  *)      MODE=(--kernel "$BIN" --kernel-adr "$KERNEL_ADR") ;;
esac
echo "[3/3] litex_term --serial-boot ${MODE[*]}"
echo "      >>> now on the Pocket: Core Settings -> Reset core -> A <<<"
exec python3 "$HERE/litex_term.py" --serial-boot "${MODE[@]}" "$PTS"
