OUTPUT_FORMAT("elf32-tradlittlemips")
OUTPUT_ARCH(mips)
ENTRY(_start)
MEMORY { rom : ORIGIN = 0xbfc00000, LENGTH = 0x1000 }
SECTIONS
{
    .text : { KEEP(*start.o(.text)) *(.text .text.*) } > rom
    .rodata : { *(.rodata .rodata.* .MIPS.abiflags) } > rom
    /DISCARD/ : { *(.data .data.* .bss .bss.* .sbss .sdata .reginfo .pdr .comment .gnu.attributes .note*) }
}
