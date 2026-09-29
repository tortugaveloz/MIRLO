#!/usr/bin/env python3
"""Turn a Quartus .rbf into the openFPGA `bitstream.rbf_r` the Pocket needs.

Every byte must be bit-reversed (bit7<->bit0, bit6<->bit1, ...) per Analogue's
packaging docs. Skipping this produces a core that fails on hardware with
"RS: Bridge not responding" even for byte-for-byte unmodified RTL, because the
fault is in packaging rather than in the design -- it builds fine and is a
plausible size, it just never boots.

Usage: package_bitstream.py <in.rbf> <out.rev> [<archive_dir> <tag>]

If an archive dir and tag are given, a copy is kept there as
`riscv-<tag>.rev` so a 20-minute build can be A/B tested later without
recompiling. Bitstreams are local-only and are never committed or pushed.
"""
import hashlib
import os
import shutil
import sys

REV = bytes(int(f"{b:08b}"[::-1], 2) for b in range(256))


def main():
    if len(sys.argv) not in (3, 5):
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]

    raw = open(src, "rb").read()
    if not raw:
        sys.exit(f"{src} is empty -- the compile did not produce a bitstream")
    open(dst, "wb").write(raw.translate(REV))

    print(f"{src} -> {dst}")
    print(f"  {len(raw)} bytes, md5(reversed)={hashlib.md5(open(dst,'rb').read()).hexdigest()[:8]}")

    if len(sys.argv) == 5:
        archive_dir, tag = sys.argv[3], sys.argv[4]
        os.makedirs(archive_dir, exist_ok=True)
        keep = os.path.join(archive_dir, f"riscv-{tag}.rev")
        shutil.copy2(dst, keep)
        print(f"  archived: {keep}")


if __name__ == "__main__":
    main()
