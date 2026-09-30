/* Audio core (MIPS, rtl/mips/mips_lite.sv): code in IMEM (the core cannot load data from IMEM -- its dBus
 * has no port there), everything else in DMEM. The game CPU writes both
 * images before releasing reset, so there is no .data copy; start.S only
 * clears .bss. */
OUTPUT_ARCH(mips)
OUTPUT_FORMAT("elf32-tradlittlemips")
ENTRY(_start)

MEMORY {
    imem (rx) : ORIGIN = 0x80000000, LENGTH = 8K
    dmem (rw) : ORIGIN = 0x80002000, LENGTH = 8K
    imem2 (rx): ORIGIN = 0x80006000, LENGTH = 8K     /* the second IMEM bank (AudioCore IMEM2_AW; MIRLO64) */
}

SECTIONS {
    .text : {
        KEEP(*(.text.start))
        *(.text .text.*)
    } > imem

    /* code for the second bank (ABI_TEXT2: naudio). Empty where there is
     * none (the Mirlo build): the section must stay empty there. */
    .text2 : {
        *(.text2 .text2.*)
    } > imem2

    .rodata : {
        *(.rodata .rodata.* .srodata .srodata.*)
    } > dmem

    .data : {
        *(.data .data.* .sdata .sdata.*)
        . = ALIGN(4);
    } > dmem

    .bss (NOLOAD) : {
        _bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > dmem

    _stack_top = ORIGIN(dmem) + LENGTH(dmem);
    ASSERT(_bss_end + 1024 <= _stack_top, "audio DMEM: less than 1 KiB left for the stack")

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.MIPS.abiflags) *(.reginfo) *(.pdr) *(.gnu.attributes) }
}
