/* Audio core: code in IMEM (the core cannot load data from IMEM -- its dBus
 * has no port there), everything else in DMEM. The game CPU writes both
 * images before releasing reset, so there is no .data copy; start.S only
 * clears .bss. */
OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY {
    imem (rx) : ORIGIN = 0x80000000, LENGTH = 8K
    dmem (rw) : ORIGIN = 0x80002000, LENGTH = 8K
}

SECTIONS {
    .text : {
        KEEP(*(.text.start))
        *(.text .text.*)
    } > imem

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

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
