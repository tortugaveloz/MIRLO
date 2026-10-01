/* Linker script for the geometry-core firmware (
 * rtl/mips/mips_geom.sv, rtl/soc/n64_geom.sv): ROM A 16 KiB
 * at 0x2000_0000 on both buses (the start code, everything read as data,
 * the small objects and the soft-float), the RAM 16 KiB at 0x2000_8000,
 * ROM B 64 KiB at 0x2001_0000 that the core can only FETCH from (the geom
 * pipeline). .rodata is a section of its own: check_isa.py reads
 * the code sections word by word. */
OUTPUT_ARCH(mips)
OUTPUT_FORMAT("elf32-tradlittlemips")
ENTRY(_start)

MEMORY {
    rom (rx)   : ORIGIN = 0x20000000, LENGTH = 0x00004000
    ram (rwx)  : ORIGIN = 0x20008000, LENGTH = 0x00004000
    rom_b (rx) : ORIGIN = 0x20010000, LENGTH = 0x00010000
}

SECTIONS
{
    .text : {
        KEEP(*(.text.start))
        *(.text.rom_a.*)                    /* code marked for ROM A (geom_texfmt.h's decoder) */
        *libcrt.a:*(.text .text.*)
        *nolibc.o(.text .text.*)
        *main.o(.text .text.*)
        *geom_mrdp_hw.o(.text .text.*)
        *geom_fixed.o(.text .text.*)
        *geom_vecmath.o(.text .text.*)
        . = ALIGN(8);
    } > rom

    /* (out of .text: lang/c/mips/check_isa.py reads .text as code) */
    .rodata : {
        *(.rodata .rodata.* .srodata .srodata.*)
        . = ALIGN(8);
    } > rom

    .data : {
        . = ALIGN(8);
        _fdata = .;
        *(.sdata .sdata.* .data .data.*)
        . = ALIGN(8);
        _edata = .;
    } > ram AT > rom
    _fdata_lma = LOADADDR(.data);

    .text_b : {
        *(.text .text.*)
    } > rom_b

    .bss (NOLOAD) : {
        . = ALIGN(8);
        _fbss = .;
        *(.sbss .sbss.* .bss .bss.* COMMON)
        . = ALIGN(8);
        _ebss = .;
    } > ram

    _end = .;
    _fstack = ORIGIN(ram) + LENGTH(ram);

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.gnu.attributes) }
}
