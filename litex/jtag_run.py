#!/usr/bin/env python3
"""Non-interactive JTAG firmware run: upload a .bin through the LiteX BIOS's
SFL serial boot over the JTAG UART relay, then log the program's output.

    jtag_run.py <relay-pty> <file.bin> [--seconds N] [--until TEXT] [--log F]

Meant for scripted hardware tests, where litex_term's interactive console
(it puts stdin in raw mode) has no terminal to talk to. Uses the same patched
LiteXTerm class as litex_term.py for the upload itself, so its fixes for the
slow, lossy transport (see jtag_uart_relay.py) apply unchanged.

Unlike jtag_boot.sh it does not wait for someone to reset the core: if the
BIOS has already gone past its serial-boot attempt and sits at the `litex>`
prompt, this types `serialboot` for it. Start openocd (openocd_rpc.cfg) and
jtag_uart_relay.py first; the relay prints its pty on its first line.
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vendor  # noqa: F401,E402  (puts the vendored LiteX on sys.path)

ns = {"__name__": "litex_term_lib"}
exec(open(os.path.join(HERE, "vendor", "litex", "litex", "tools", "litex_term.py")).read(), ns)


class _NoConsole:
    """LiteXTerm builds a termios console in __init__; there is none here."""
    def configure(self): pass
    def unconfigure(self): pass
    def getkey(self): time.sleep(1); return b""
    def escape_char(self, b): return False
    def handle_escape(self, b): return b""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pty")
    ap.add_argument("bin")
    ap.add_argument("--adr", default="0x40000000")
    ap.add_argument("--seconds", type=float, default=120.0,
                    help="stop logging this long after the upload finished")
    ap.add_argument("--until", default=None,
                    help="stop as soon as this text appears after the upload")
    ap.add_argument("--log", default=None, help="also append all output here")
    args = ap.parse_args()

    ns["Console"] = _NoConsole
    log = open(args.log, "ab") if args.log else None

    class Tee:
        # Sticky flags, not searches of `buf`: a large upload's progress bar
        # scrolls the request marker out of the 64 KB window, and this once
        # read that as "no request yet" and typed `serialboot` into the middle
        # of the SFL transfer.
        def __init__(self):
            self.buf = b""
            self.requested = False
            self.done = False
        def write(self, b):
            if isinstance(b, str):
                b = b.encode()
            tail = self.buf[-64:] + b
            if b"[LITEX-TERM] Received" in tail: self.requested = True
            if b"[LITEX-TERM] Done." in tail: self.done = True
            sys.__stdout__.buffer.write(b)
            sys.__stdout__.flush()
            if log:
                log.write(b); log.flush()
            self.buf = (self.buf + b)[-65536:]
        def flush(self): pass

    tee = Tee()

    class Out:  # the reader thread writes via sys.stdout.buffer and print()
        buffer = tee
        def write(self, s): tee.write(s)
        def flush(self): pass

    sys.stdout = Out()

    term = ns["LiteXTerm"](serial_boot=True, kernel_image=args.bin,
                            kernel_address=args.adr, json_images=None, safe=False)
    term.open(args.pty, 115200)
    term.start_reader()

    t_kick = time.time()
    kicks = 0
    while not tee.done:
        # A BIOS already at its prompt never asks for a kernel on its own.
        if time.time() - t_kick > 8 and not tee.requested:
            term.port.write(b"\nserialboot\n")
            kicks += 1
            t_kick = time.time()
            if kicks > 10:
                print("\n[jtag_run] no serial-boot request after 10 kicks, giving up")
                return 2
        time.sleep(0.2)

    t_end = time.time() + args.seconds
    start = len(tee.buf)
    while time.time() < t_end:
        if args.until and args.until.encode() in tee.buf[max(0, start - 64):]:
            break
        time.sleep(0.2)
    term.reader_alive = False
    return 0


if __name__ == "__main__":
    sys.exit(main())
