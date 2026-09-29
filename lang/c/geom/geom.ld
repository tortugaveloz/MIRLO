/* Linker script for the geometry-core firmware.
 *
 * Split ROM + RAM: the geometry core boots from a read-only ROM (which keeps
 * its synthesis-time $readmemh init on this Cyclone V; a byte-writable RAM
 * does not -- see litex/analogue_pocket.py add_geometry_subsystem) and keeps
 * its working set (.data, .bss, stack) in a separate RAM. .data is loaded in
 * ROM (LMA) and copied to RAM (VMA) by start.S.
 */
OUTPUT_FORMAT("elf32-littleriscv", "elf32-littleriscv", "elf32-littleriscv")
OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY {
    rom (rx)   : ORIGIN = 0x20000000, LENGTH = 0x00004000
    rom_b (rx) : ORIGIN = 0x20004000, LENGTH = 0x00004000
    ram (rwx)  : ORIGIN = 0x20008000, LENGTH = 0x00004000
}

SECTIONS
{
    /* ROM B (litex/analogue_pocket.py): 16 KiB the core can only FETCH from --
     * it has no data port, so nothing but code may live here (no .rodata;
     * -fno-jump-tables keeps switch tables out of .text). Listed first so
     * these input sections match here rather than in .text below. */
    .text_b : {
        *(.text.emit_clipped*)
        *(.text.emit_setup_and_stream*)
        *(.text.geom_reset*)
        *(.text.clip_outcode*)
        *(.text.gm_mat4_mul_fx*)
        *(.text.main*)
        *(.text.fx_div_ii*)
        *(.text.mrdp_emit_fx*)
        *(.text.mrdp_setup_edges*)
        *(.text.mrdp_edges_of*)
        *(.text.mrdp_put_tri_g*)
        *(.text.mrdp_factors*)
    } > rom_b

    .text : {
        KEEP(*(.text.start))
        /* Per-triangle hot path first, contiguous and in call order. The geom
         * core's I-cache is 4 KB direct-mapped against ~15 KB of code, so two
         * hot functions whose addresses collide modulo 4 KB evict each other on
         * every call -- and where they land otherwise shifts with every
         * unrelated edit (one such shift cost +34% cycles on a clip-heavy scene
         * in sim/geom_full with no change to the clip code). Pinning the order
         * keeps the hot set's layout fixed and free of self-conflicts. */
        *(.text.emit_triangle*)
        *(.text.geom_triangle_rejects_e*)
        *(.text.edges_of*)
        *(.text.to_edge_fixed*)
        *(.text.offscreen_guard*)
        *(.text.fx_to_int_round*)
        *(.text.make_screen_vertex*)
        *(.text.emit_setup_and_stream*)
        *(.text.geom_triangle_setup_e*)
        *(.text.round_to_float*)
        *(.text.blend_fx*)
        *(.text.fx_recip*)
        *(.text.fx_div_norm*)
        *(.text.emit_clipped*)
        *(.text.lerp1*)
        *(.text.make_clip_vtx*)
        *(.text.vpar_to_fx*)
        *(.text .text.*)
        *(.rodata .rodata.* .srodata .srodata.*)
        . = ALIGN(8);
    } > rom

    .data : {
        . = ALIGN(8);
        _fdata = .;
        __global_pointer$ = . + 0x400;
        *(.sdata .sdata.* .data .data.*)
        . = ALIGN(8);
        _edata = .;
    } > ram AT > rom
    _fdata_lma = LOADADDR(.data);

    .bss (NOLOAD) : {
        . = ALIGN(8);
        _fbss = .;
        *(.sbss .sbss.* .bss .bss.* COMMON)
        . = ALIGN(8);
        _ebss = .;
    } > ram

    _end = .;
    _fstack = ORIGIN(ram) + LENGTH(ram);
}
