# Geometry-core firmware: standalone rv32im / ilp32 -- NO hardware FPU (see
# rtl/geom/VexRiscvGeom.v's generator, GenCoreGeomCfu.scala: the real N64 RSP
# had none either, only the main CPU did -- see lang/c/geom/geom_fixed.h's
# header for the full rationale). Every fp32 op in this firmware is really
# pure-integer S15.16 fixed point underneath (geom_fixed.c/geom_vecmath.c);
# `float` stays the C-level type at call sites, it just never reaches a real
# fadd.s/fmul.s/fdiv.s or a soft-float libgcc call -- if one ever did sneak
# back in, this -nostdlib build would fail to LINK (no soft-float available),
# which is a real safety net, not just a hope. Self-contained: does not use
# LiteX's common.mak (that targets the game CPU's rv32imafc / ilp32f).
#
# Two-pass bootstrap (like any LiteX firmware): the CSR addresses come from
# litex/build/.../generated/csr.h, so run the LiteX gateware build once first
# (it seeds geom_sram with a spin loop), then `make` here, then re-run the
# gateware build pointing geom_sram init at build/geom.init.
#
# The FPU+CFU hybrid variant this superseded (rv32imf, tagged
# geom-fpu-cfu-hybrid @ b651ab5) is kept as a fallback, not deleted -- its
# own Scala generator (GenCoreGeomFpuCfu.scala) and rtl/geom/VexRiscvGeom.v
# snapshot live at that tag if the fixed-point rewrite ever needs reverting.

CROSS   ?= $(HOME)/local/opt/riscv-gcc/bin/riscv-none-elf-
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy

GEN_CSR  = ../../../litex/build/litex/software/include/generated/csr.h

ARCHFLAGS = -march=rv32i_zmmul_zicsr -mabi=ilp32   # no divider in the geom CPU: multiply only (Zmmul)
# -fno-jump-tables: measured smaller here (the alternative -- a compare
# chain -- beats a jump table's own data-table overhead for this firmware's
# actual switch statements, e.g. the GDL opcode dispatch). A real, measured
# ROM-budget lever like nolibc.o's -O1 override below, not a default guess --
# see this Makefile's git history for the numbers (1920 bytes over the geom
# core's 16KB ROM at the first real link attempt, down to 96 after this plus
# the -O1 override plus de-duplicating what used to be `static inline` code
# across translation units -- see geom_fixed.h's own header comment for that
# part of the story).
CFLAGS    = $(ARCHFLAGS) -fsigned-char -O2 -g -fno-jump-tables -ffreestanding -nostdlib -fno-pic -mno-relax \
            -Wall -Wextra -ffunction-sections -fdata-sections -std=gnu11 \
            -DGEOM_MRDP -Ibuild $(CFLAGS_EXTRA)
LDFLAGS   = $(ARCHFLAGS) -nostdlib -Wl,--gc-sections -Wl,-Map,build/geom.map -T geom.ld

OBJS = build/start.o build/nolibc.o build/main.o \
       build/geom_pipeline.o build/geom_triangle.o \
       build/geom_fixed.o build/geom_vecmath.o build/geom_mrdp_hw.o

all: build/geom.bin build/geom.init

# ---- host tests: the fixed-point maths and the VPU C model ----
HOSTCC ?= cc
test/test_geom_fixed: test/test_geom_fixed.c geom_fixed.c geom_fixed.h
	$(HOSTCC) -O2 -Wall -Wextra -DGEOM_HOST_TEST -I. test/test_geom_fixed.c geom_fixed.c -lm -o $@

test/test_vpar: test/test_vpar.c geom_vpar.h geom_fixed.c
	$(HOSTCC) -O2 -Wall -Wextra -DGEOM_HOST_TEST -I. test/test_vpar.c geom_fixed.c -lm -o $@

hosttest: test/test_geom_fixed test/test_vpar
	./test/test_geom_fixed
	./test/test_vpar

build:
	mkdir -p build

# Extract just the CSR base addresses / field offsets this firmware needs.
build/generated_csr_addrs.h: $(GEN_CSR) | build
	@echo "/* auto-extracted from $(GEN_CSR) -- do not edit */" > $@
	@echo "#ifndef GEOM_GENERATED_CSR_ADDRS_H" >> $@
	@echo "#define GEOM_GENERATED_CSR_ADDRS_H" >> $@
	@grep -E '#define (CSR_BASE|CSR_(MAILBOX|MRDP)_)' $(GEN_CSR) >> $@
	@echo "#endif" >> $@

build/%.o: %.S build/generated_csr_addrs.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: %.c build/generated_csr_addrs.h | build
	$(CC) $(CFLAGS) -c $< -o $@

# -O2 throughout since the geom ROM grew to 24 KiB (2026-09-26): ~15 % fewer
# cycles than -Os in sim/geom_full. It used to be -Os (and -O1 for nolibc.c)
# to fit 16 KiB -- see this Makefile's git history for that ROM-fitting story.
# -fno-jump-tables stays: a jump table is a D-cache read from the ROM, and
# with it the same frames took ~30 % longer.

build/geom.elf: $(OBJS) geom.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

build/geom.bin: build/geom.elf
	$(OBJCOPY) -O binary $< $@

# 32-bit-word hex, one per line -- ready for $readmemh / LiteX add_ram(contents=).
build/geom.init: build/geom.bin
	od -An -tx4 -v $< | tr -s ' ' '\n' | sed '/^$$/d' > $@

clean:
	rm -rf build test/test_geom

.PHONY: all clean hosttest

# geom_triangle.c at -O3: its fixed-count loops (3 vertices, 3 channels) unrolled
# -- sim/geom_full -3 % (JRB) / -4 % (Bowser), +264 bytes of ROM
build/geom_triangle.o: CFLAGS := $(subst -O2,-O3,$(CFLAGS))
