/* MIRLO's map on the MIPS SoC (rtl/soc/mirlo_mips.sv) -- the same as the
 * RISC-V SoC's, less the LiteX-only regions (the BIOS ROM, its SRAM, the
 * PLIC/CLINT). */
MEMORY {
	bootrom : ORIGIN = 0xbfc00000, LENGTH = 0x00001000
	main_ram : ORIGIN = 0x40000000, LENGTH = 0x04000000
	video_framebuffer : ORIGIN = 0x40c00000, LENGTH = 0x00800000
	geom_rom : ORIGIN = 0x20000000, LENGTH = 0x00008000
	geom_ram : ORIGIN = 0x20008000, LENGTH = 0x00004000
	audio : ORIGIN = 0x80000000, LENGTH = 0x00008000
	csr : ORIGIN = 0xf0000000, LENGTH = 0x00010000
}
REGION_ALIAS("REGION_TEXT", main_ram);
REGION_ALIAS("REGION_RODATA", main_ram);
REGION_ALIAS("REGION_DATA", main_ram);
REGION_ALIAS("REGION_BSS", main_ram);
REGION_ALIAS("REGION_HEAP", main_ram);
REGION_ALIAS("REGION_STACK", main_ram);
