#!/usr/bin/env python3
"""Fast lossless JTAG-UART relay for LiteX's AlteraJTAG JTAGPHY, via openocd TCL-RPC.

Root cause of the earlier 0%-TX: the JTAGPHY XFER FSM
    self.comb += fsm.reset.eq(jtag.reset | jtag.capture)
resets on every Capture-DR, clearing the `ready` bit (bit0 out = target accepted
the byte) before it can be observed. Fix: keep the TAP in Pause-DR between scans
(`-endstate DRPAUSE`); Pause-DR -> Exit2-DR -> Shift-DR never hits Capture-DR, so
the FSM (and `ready`) persist. Verified: readback goes 0x08a then steady 0x08b.

Throughput: batch up to BATCH data words + 1 trailing filler word into a single
`drscan` (one RPC round-trip shifts BATCH*10+10 bits contiguously in Shift-DR).
For a data word at field i, its accept/reject shows up in readback word i+1
bit0 -- the trailing filler guarantees every data byte's ack lands in the same
readback. A rejected byte (bit0==0, target FIFO was full) is requeued and the
rest of that batch is deferred to preserve byte order.

Per 10-bit word, LSB first:  bit0 in = host ready (always 1) / out = `ready`
(prev word's byte accepted);  bits1..8 = data;  bit9 in = TX valid /
out = `valid` (RX byte present, in bits1..8).

Exposes a PTY (path on line 1) so litex_term uses a plain serial device and
never spawns its own openocd. Needs openocd already up: tap defined + `init`,
TCL-RPC on :6666.
"""
import collections, os, pty, socket, sys, threading, time, tty

RPC   = ("127.0.0.1", 6666)
TAP   = "cv5.tap"
SUB   = b"\x1a"
BATCH = 24            # data words per drscan (+1 filler)


class OO:
    def __init__(self):
        self.s = socket.create_connection(RPC, 5)
        self.s.settimeout(15)
        self.buf = b""

    def cmd(self, c):
        self.s.sendall(c.encode() + SUB)
        while SUB not in self.buf:
            d = self.s.recv(65536)
            if not d:
                raise ConnectionError("openocd RPC closed")
            self.buf += d
        line, self.buf = self.buf.split(SUB, 1)
        return line.decode(errors="replace").strip()


def main():
    m, sfd = pty.openpty()
    tty.setraw(m)
    print(os.ttyname(sfd), flush=True)

    oo = OO()
    oo.cmd("irscan %s 0x00e" % TAP)
    oo.cmd("drscan %s 10 0x001 -endstate DRPAUSE" % TAP)          # prime -> Pause-DR
    probe = [oo.cmd("drscan %s 10 0x001 -endstate DRPAUSE" % TAP) for _ in range(3)]
    sys.stderr.write("jtag_relay: primed, probe=%s\n" % probe)
    sys.stderr.flush()

    txq = collections.deque()

    def reader():
        while True:
            try:
                d = os.read(m, 65536)
            except OSError:
                return
            if not d:
                return
            txq.extend(d)

    threading.Thread(target=reader, daemon=True).start()

    st = {"tx": 0, "rx": 0, "resend": 0, "calls": 0}
    last_log = time.time()

    while True:
        n = min(len(txq), BATCH)
        bytes_out = [txq[i] for i in range(n)]          # peek, don't pop yet
        fields = []
        for b in bytes_out:
            fields.append("10 0x%03X" % (0x001 | ((b & 0xFF) << 1) | 0x200))
        fields.append("10 0x001")                        # trailing filler -> acks
        if n == 0:
            # pure RX drain: a few filler words
            fields = ["10 0x001"] * 6

        try:
            resp = oo.cmd("drscan %s %s -endstate DRPAUSE" % (TAP, " ".join(fields)))
        except Exception as e:
            sys.stderr.write("jtag_relay: RPC error: %s\n" % e)
            sys.stderr.flush()
            return
        st["calls"] += 1
        words = []
        for tok in resp.split():
            try:
                words.append(int(tok, 16))
            except ValueError:
                pass

        # RX: any word with bit9 set carries a byte
        for w in words:
            if w & 0x200:
                os.write(m, bytes([(w >> 1) & 0xFF]))
                st["rx"] += 1

        # TX acks: data field i -> readback word i+1, bit0
        if n:
            accepted = 0
            for i in range(n):
                if i + 1 < len(words) and (words[i + 1] & 0x001):
                    accepted += 1
                else:
                    break                                # keep byte order: stop at first reject
            for _ in range(accepted):
                txq.popleft()
            st["tx"] += accepted
            if accepted < n:
                st["resend"] += 1
                time.sleep(1e-3)                          # let the target FIFO drain

        if n == 0:
            time.sleep(0.02)   # was 5e-4: ~2000 idle RPC calls/sec was needlessly
                                # hammering openocd/the USB-Blaster while nothing
                                # was queued (e.g. while a human/script is still
                                # navigating the Pocket's menu before triggering
                                # Reset core) -- tried after observing every
                                # upload attempt this session stall at nearly the
                                # same small byte count regardless of frame size,
                                # suggesting cumulative connection wear rather
                                # than a per-frame issue. 20ms keeps idle latency
                                # low enough to still feel responsive.

        now = time.time()
        if now - last_log > 3:
            sys.stderr.write("jtag_relay: tx=%d rx=%d resend=%d calls=%d q=%d\n"
                             % (st["tx"], st["rx"], st["resend"], st["calls"], len(txq)))
            sys.stderr.flush()
            last_log = now


if __name__ == "__main__":
    main()
