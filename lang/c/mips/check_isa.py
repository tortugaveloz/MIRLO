#!/usr/bin/env python3
"""Fail if a firmware for mips_lite (rtl/mips/mips_lite.sv) has an instruction
that core does not implement -- it would run as a no-op there, silently.

    check_isa.py [--div] [--clz] <objcopy> <elf> <section>...

Reads each code section's words (little-endian) and checks them against the
core's decode: MIPS I integer without traps, mul, movn/movz, lwl/lwr,
mult/multu, HI/LO, the CFU (major opcode 0x1F), and with --div / --clz the
core's WITH_DIV / WITH_CLZ instructions. Data must not sit in these sections
(the linker scripts keep .rodata out of them).
"""
import os
import struct
import subprocess
import sys
import tempfile

SPECIAL = {0x00, 0x02, 0x03, 0x04, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x10, 0x11, 0x12, 0x13,
           0x18, 0x19, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B}
REGIMM = {0x00, 0x01, 0x10, 0x11}
MAJOR = set(range(0x02, 0x10)) | {0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2B}


def main():
    args = sys.argv[1:]
    div = '--div' in args
    clz = '--clz' in args
    args = [a for a in args if not a.startswith('--')]
    objcopy, elf, sections = args[0], args[1], args[2:]
    special = SPECIAL | ({0x1A, 0x1B} if div else set())
    special2 = {0x02} | ({0x20, 0x21} if clz else set())
    bad = 0
    for sec in sections:
        with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as t:
            path = t.name
        try:
            addr = int(subprocess.check_output([objcopy.replace('objcopy', 'objdump'), '-h', elf], text=True)
                       .split(' ' + sec + ' ')[1].split()[2], 16)
            subprocess.check_call([objcopy, '-O', 'binary', '-j', sec, elf, path])
            data = open(path, 'rb').read()
        finally:
            os.unlink(path)
        for k in range(0, len(data) - 3, 4):
            w, = struct.unpack_from('<I', data, k)
            op, rt, fn = w >> 26, (w >> 16) & 31, w & 63
            ok = (op == 0x00 and fn in special) or (op == 0x01 and rt in REGIMM) or \
                 (op == 0x1C and fn in special2) or op in MAJOR
            if not ok:
                bad += 1
                if bad <= 20:
                    print(f'{elf}: {sec} {addr + k:08x}: {w:08x} not implemented by mips_lite', file=sys.stderr)
    if bad:
        print(f'{elf}: {bad} unimplemented instruction(s)', file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
