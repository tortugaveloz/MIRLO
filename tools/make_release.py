#!/usr/bin/env python3
"""Pack the SD-card release zip: the core as it sits on the Pocket's SD card.

    tools/make_release.py <mirlo.rev> <out.zip>

<mirlo.rev> is the bit-reversed bitstream (tools/package_bitstream.py). The
zip holds, from the SD card's root:

    Cores/tortuga.Mirlo/     the core definition (pkg/pocket), mirlo.rev,
                             LICENSE.txt, NOTICE.txt
    Platforms/mirlo.json     the platform (category Computer) and its image
    Platforms/_images/mirlo.bin
    Assets/mirlo/common/     empty: where games go

so a user unzips it onto the card's root and is done. Entries are written in
a fixed order with a fixed date, so the same inputs give the same zip.
"""
import os
import sys
import zipfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
PKG = os.path.join(ROOT, "pkg", "pocket")
CORE = "Cores/tortuga.Mirlo"
DATE = (2026, 1, 1, 0, 0, 0)


def add(z, name, data):
    info = zipfile.ZipInfo(name, DATE)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    z.writestr(info, data)


def add_dir(z, name):
    info = zipfile.ZipInfo(name.rstrip("/") + "/", DATE)
    info.external_attr = (0o40755 << 16) | 0x10
    z.writestr(info, b"")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    rev, out = sys.argv[1], sys.argv[2]
    files = []
    for d, _, names in os.walk(PKG):
        for n in names:
            p = os.path.join(d, n)
            rel = os.path.relpath(p, PKG).replace(os.sep, "/")
            if rel.endswith(".rev"):
                continue                       # the bitstream comes from the argument
            files.append((rel, p))
    files.append((CORE + "/mirlo.rev", rev))
    files.append((CORE + "/LICENSE.txt", os.path.join(ROOT, "LICENSE")))
    files.append((CORE + "/NOTICE.txt", os.path.join(ROOT, "NOTICE")))
    files.sort()
    dirs = sorted({"/".join(r.split("/")[:i]) for r, _ in files for i in range(1, r.count("/") + 1)}
                  | {"Assets", "Assets/mirlo", "Assets/mirlo/common"})
    with zipfile.ZipFile(out, "w") as z:
        for d in dirs:
            add_dir(z, d)
        for rel, p in files:
            with open(p, "rb") as f:
                add(z, rel, f.read())
    print(f"{out}: {len(files)} files")
    for rel, _ in files:
        print("  " + rel)


if __name__ == "__main__":
    main()
