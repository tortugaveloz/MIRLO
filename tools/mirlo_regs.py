#!/usr/bin/env python3
"""Mirlo's register map on the MIPS SoC (rtl/soc/mirlo_mips.sv): the one table
the RTL decode and the firmware's headers come from.

    mirlo_regs.py  -> rtl/soc/mirlo_regs.svh           (the decode's offsets)
                      lang/mips/include/generated/csr.h, soc.h, mem.h

It replaces LiteX's generated csr.h: the blocks and registers keep the names
the firmware has always used (apf_audio_out_write(), mrdp_status_read(),
timer0_uptime_cycles_read(), ...), with the same accessors -- <reg>_read(),
<reg>_write(), and per field <reg>_<field>_read() / _extract() -- so
lang/c and the games build against it unchanged.

All registers are 32-bit words at CSR_BASE (0xF000_0000) + offset; a 64-bit
register is two words, the high one first (LiteX's order).
"""
import os

CSR_BASE = 0xF000_0000
CLOCK_HZ = 62_832_000

# block: (base offset, [(name, offset, access, words, fields)]), fields: [(name, lsb, size)]
R, W, RW = "r", "w", "rw"
BLOCKS = [
    ("APF_AUDIO", 0x0000, [
        ("OUT",          0x00, W,  1, []),
        ("PLAYBACK_EN",  0x04, RW, 1, []),
        ("BUFFER_FLUSH", 0x08, W,  1, []),
        ("BUFFER_FILL",  0x0C, R,  1, []),
    ]),
    ("APF_BRIDGE", 0x0100, [
        ("REQUEST_READ",        0x00, W,  1, []),
        ("REQUEST_WRITE",       0x04, W,  1, []),
        ("REQUEST_GETFILE",     0x08, W,  1, []),
        ("REQUEST_OPENFILE",    0x0C, W,  1, []),
        ("SLOT_ID",             0x10, RW, 1, []),
        ("DATA_OFFSET",         0x14, RW, 1, []),
        ("TRANSFER_LENGTH",     0x18, RW, 1, []),
        ("RAM_DATA_ADDRESS",    0x1C, RW, 1, []),
        ("FILE_SIZE",           0x20, RW, 1, []),   # read: the slot's size; write: set it
        ("STATUS",              0x24, R,  1, []),   # 1: a command completed (a read clears it)
        ("CURRENT_ADDRESS",     0x28, R,  1, []),
        ("COMMAND_RESULT_CODE", 0x2C, R,  1, []),
        ("BOOT_READY",          0x30, RW, 1, []),   # (kept for the name: the SDRAM tells the Pocket)
        ("HOST_STATE",          0x34, R,  1, [("RESET_N", 0, 1), ("LOADED", 1, 1)]),
    ]),
    ("APF_INPUT", 0x0200, [
        (f"CONT{i}_{k}", 0x00 + 4 * (4 * j + i - 1), R, 1, [])
        for j, k in enumerate(["KEY", "JOY", "TRIG"]) for i in range(1, 5)
    ]),
    ("APF_INTERACT", 0x0300, [
        ("INTERACT0",         0x00, RW, 1, []),     # a write clears INTERACT_CHANGED0
        ("INTERACT_CHANGED0", 0x04, R,  1, []),
        ("INTERACT1",         0x08, RW, 1, []),
        ("INTERACT_CHANGED1", 0x0C, R,  1, []),
    ]),
    ("APF_VIDEO", 0x0400, [
        # a read clears VBLANK_TRIGGERED (LiteX's CSRStatus: its `we` is the read)
        ("VIDEO", 0x00, R, 1, [("VBLANK_STATUS", 0, 1), ("VBLANK_TRIGGERED", 1, 1), ("FRAME_COUNTER", 2, 30)]),
    ]),
    ("CTRL", 0x0500, [
        ("RESET",      0x00, W,  1, []),            # (no soft reset: Quit + relaunch)
        ("SCRATCH",    0x04, RW, 1, []),
        ("BUS_ERRORS", 0x08, R,  1, []),            # the game CPU's accesses to nothing
    ]),
    ("MAILBOX", 0x0600, [
        ("GAME_MSG",  0x00, RW, 1, []),
        ("GAME_KICK", 0x04, W,  1, []),
        ("GEOM_ACK",  0x08, W,  1, []),
        ("GEOM_MSG",  0x0C, RW, 1, []),
        ("GEOM_KICK", 0x10, W,  1, []),
        ("GAME_ACK",  0x14, W,  1, []),
        ("STATUS",    0x18, R,  1, [("GEOM_PENDING", 0, 1), ("GAME_PENDING", 1, 1)]),
    ]),
    ("MRDP", 0x0700, [
        ("CMD_DATA",    0x00, W, 1, []),
        ("CMD_STATUS",  0x04, R, 1, [("LEVEL", 0, 12), ("FULL", 12, 1)]),
        ("CMD_DROPPED", 0x08, R, 1, []),
        ("SYNC_COUNT",  0x0C, R, 1, []),
        ("LOAD_COUNT",  0x10, R, 1, []),
        ("STATUS",      0x14, R, 1, [("IDLE", 0, 1), ("UNKNOWN_OPS", 16, 16)]),
    ]),
    ("TIMER0", 0x0800, [
        ("UPTIME_LATCH",  0x00, W, 1, []),
        ("UPTIME_CYCLES", 0x04, R, 2, []),          # 0x04 high word, 0x08 low
    ]),
    ("UART", 0x0900, [
        ("RXTX",       0x00, RW, 1, []),            # write: a byte out (dropped if full); read: a byte in
        ("TXFULL",     0x04, R,  1, []),
        ("RXEMPTY",    0x08, R,  1, []),
        ("EV_PENDING", 0x0C, RW, 1, [("TX", 0, 1), ("RX", 1, 1)]),   # (polling only: always 0)
    ]),
    ("VIDEO_FRAMEBUFFER", 0x0A00, [
        ("DMA_BASE",   0x00, RW, 1, []),            # latched at the frame boundary
        ("DMA_LENGTH", 0x04, RW, 1, []),            # (the mode sets the length)
        ("DMA_ENABLE", 0x08, RW, 1, []),
        ("DMA_OFFSET", 0x0C, R,  1, []),
        ("VTG_ENABLE", 0x10, RW, 1, []),
        ("VTG_MODE",   0x14, RW, 1, []),            # 0: 268 x 240, 1: 320 x 240 (scaler slot 1)
    ]),
]
MRDP_CMD_FIFO_DEPTH = 2048      # CMD_STATUS.LEVEL is its width + 1 bits: 12

# soc.h's constants and mem.h's regions (LiteX's names)
SOC = [("CONFIG_CLOCK_FREQUENCY", CLOCK_HZ), ("ROM_BOOT_ADDRESS", 0x4000_0000),
       ("VIDEO_FRAMEBUFFER_BASE", 0x40C0_0000), ("VIDEO_FRAMEBUFFER_HRES", 268), ("VIDEO_FRAMEBUFFER_VRES", 240),
       ("VIDEO_FRAMEBUFFER_DEPTH", 16), ("MAX_DISPLAY_WIDTH", 268), ("MAX_DISPLAY_HEIGHT", 240)]
REGIONS = [("BOOTROM", 0xBFC0_0000, 0x1000), ("MAIN_RAM", 0x4000_0000, 0x0400_0000),
           ("VIDEO_FRAMEBUFFER", 0x40C0_0000, 0x0080_0000), ("GEOM_ROM", 0x2000_0000, 0x8000),
           ("GEOM_RAM", 0x2000_8000, 0x4000), ("AUDIO", 0x8000_0000, 0x8000), ("CSR", CSR_BASE, 0x1_0000)]


def c_header():
    """#defines for C and assembly; the accessors only for C."""
    d = ["/* generated by tools/mirlo_regs.py -- do not edit (Mirlo's MIPS register map) */",
         "#ifndef __GENERATED_CSR_H", "#define __GENERATED_CSR_H",
         "#ifndef CSR_BASE", "#ifdef __ASSEMBLER__", f"#define CSR_BASE 0x{CSR_BASE:08x}", "#else",
         f"#define CSR_BASE 0x{CSR_BASE:08x}L", "#endif", "#endif",
         "/* offsets from CSR_BASE (assembly: lui 0xF000, then these) */"]
    f = ["#ifndef __ASSEMBLER__", "#include <stdint.h>",
         "#ifndef CSR_ACCESSORS_DEFINED",
         "#define CSR_ACCESSORS_DEFINED",
         "static inline uint32_t csr_read_simple(unsigned long a) { return *(volatile uint32_t *)a; }",
         "static inline void csr_write_simple(uint32_t v, unsigned long a) { *(volatile uint32_t *)a = v; }",
         "#endif"]
    for blk, base, regs in BLOCKS:
        b = blk.lower()
        d.append(f"\n/* {blk} */")
        d.append(f"#define CSR_{blk}_BASE (CSR_BASE + 0x{base:x})")
        f.append(f"\n/* {blk} */")
        for name, off, acc, words, fields in regs:
            n = f"{b}_{name.lower()}"
            addr = f"CSR_{blk}_{name}_ADDR"
            d.append(f"#define CSR_{blk}_{name}_OFS 0x{base + off:x}")
            d.append(f"#define {addr} (CSR_BASE + 0x{base + off:x})")
            d.append(f"#define CSR_{blk}_{name}_SIZE {words}")
            if words == 2:
                f.append(f"static inline uint64_t {n}_read(void) {{ uint64_t h = csr_read_simple({addr}); "
                         f"return (h << 32) | csr_read_simple({addr} + 4); }}")
            else:
                f.append(f"static inline uint32_t {n}_read(void) {{ return csr_read_simple({addr}); }}")
            if "w" in acc:
                f.append(f"static inline void {n}_write(uint32_t v) {{ csr_write_simple(v, {addr}); }}")
            for fname, lsb, size in fields:
                fn = f"{n}_{fname.lower()}"
                mask = (1 << size) - 1
                d.append(f"#define CSR_{blk}_{name}_{fname}_OFFSET {lsb}")
                d.append(f"#define CSR_{blk}_{name}_{fname}_SIZE {size}")
                f.append(f"static inline uint32_t {fn}_extract(uint32_t r) {{ return (r >> {lsb}) & 0x{mask:x}u; }}")
                f.append(f"static inline uint32_t {fn}_read(void) {{ return {fn}_extract({n}_read()); }}")
                if "w" in acc:
                    f.append(f"static inline uint32_t {fn}_replace(uint32_t r, uint32_t v) "
                             f"{{ return (r & ~(0x{mask:x}u << {lsb})) | ((v & 0x{mask:x}u) << {lsb}); }}")
                    f.append(f"static inline void {fn}_write(uint32_t v) {{ {n}_write({fn}_replace({n}_read(), v)); }}")
    f.append("#endif /* __ASSEMBLER__ */")
    return "\n".join(d + [""] + f + ["", "#endif"]) + "\n"


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    sv = ["// generated by tools/mirlo_regs.py -- do not edit",
          "// byte offsets from CSR_BASE (0xF000_0000); the decode uses bits [15:2]"]
    for blk, base, regs in BLOCKS:
        for name, off, _, words, _ in regs:
            sv.append(f"localparam logic [15:0] R_{blk}_{name} = 16'h{base + off:04X};")
    sv.append(f"localparam int MRDP_CMD_FIFO_DEPTH = {MRDP_CMD_FIFO_DEPTH};")
    open(os.path.join(root, "rtl", "soc", "mirlo_regs.svh"), "w").write("\n".join(sv) + "\n")
    gen = os.path.join(root, "lang", "mips", "include", "generated")
    os.makedirs(gen, exist_ok=True)
    open(os.path.join(gen, "csr.h"), "w").write(c_header())
    soc = ["/* generated by tools/mirlo_regs.py -- do not edit */",
           "#ifndef __GENERATED_SOC_H", "#define __GENERATED_SOC_H"]
    for k, v in SOC:
        soc += [f"#ifndef {k}", f"#define {k} {v}", "#endif"]
    soc += ["#define CONFIG_CPU_MIPS 1", "#endif"]
    open(os.path.join(gen, "soc.h"), "w").write("\n".join(soc) + "\n")
    mem = ["/* generated by tools/mirlo_regs.py -- do not edit */",
           "#ifndef __GENERATED_MEM_H", "#define __GENERATED_MEM_H"]
    for k, o, n in REGIONS:
        mem += [f"#ifndef {k}_BASE", f"#define {k}_BASE 0x{o:08x}L", f"#define {k}_SIZE 0x{n:08x}", "#endif"]
    mem.append("#endif")
    open(os.path.join(gen, "mem.h"), "w").write("\n".join(mem) + "\n")
    print("rtl/soc/mirlo_regs.svh, lang/mips/include/generated/{csr,soc,mem}.h")


if __name__ == "__main__":
    main()
