# Geometry-core firmware for mips_geom (rtl/mips/mips_geom.sv): MIPS32's
# integer subset with a divider and clz, NO FPU (the real N64 RSP had none
# either, only the main CPU did -- see geom_fixed.h's header). Every fp32 op
# here is pure-integer S15.16 fixed point underneath (geom_fixed.c,
# geom_vecmath.c); `float` stays the C-level type at call sites. The little
# soft-float there is comes from compiler-rt (lang/mips/build/lite/libcrt.a),
# in ROM A. The CFU instructions: geom_vpar.h / geom_fixed.h (lang/c/mips/
# mips_cfu.h); the D-cache flush: geom_dcache.h.
#
# The firmware is baked into the bitstream (tools/mips_inits.py puts
# build/geom.bin in the geom core's ROMs), so a change here needs a Quartus
# run. The register addresses come from lang/mips/include/generated/csr.h
# (tools/mirlo_regs.py).
#
# Needs: make -f ../../mips/lib.mk VARIANT=lite (picolibc's headers, compiler-rt).

include ../../mips/mips.mk
B         = build
CC        = $(MIPS_GCC)
OBJCOPY   = $(MIPS_OBJCOPY)
GEN_CSR   = ../../mips/include/generated/csr.h
CRT_LIB   = ../../mips/build/lite/libcrt.a

# -fno-jump-tables: a jump table is a D-cache read from the ROM, and with it
# the same frames took ~30 % longer (sim/geom_full). -O2 throughout: ~15 %
# fewer cycles than -Os.
CFLAGS    = $(MIPS_LITE_CFLAGS) -I../mips -fsigned-char -O2 -g -fno-jump-tables -ffreestanding -nostdlib -fno-pic \
            -fno-strict-aliasing -Wall -Wextra -ffunction-sections -fdata-sections -std=gnu11 \
            -DGEOM_MRDP -I$(B) $(CFLAGS_EXTRA)

OBJS = $(B)/start_mips.o $(B)/nolibc.o $(B)/main.o \
       $(B)/geom_pipeline.o $(B)/geom_triangle.o \
       $(B)/geom_fixed.o $(B)/geom_vecmath.o $(B)/geom_mrdp_hw.o

all: $(B)/geom.bin $(B)/geom.init

# ---- host tests: the fixed-point maths and the VPU C model ----
HOSTCC ?= cc
test/test_geom_fixed: test/test_geom_fixed.c geom_fixed.c geom_fixed.h
	$(HOSTCC) -O2 -Wall -Wextra -DGEOM_HOST_TEST -I. test/test_geom_fixed.c geom_fixed.c -lm -o $@

test/test_vpar: test/test_vpar.c geom_vpar.h geom_fixed.c
	$(HOSTCC) -O2 -Wall -Wextra -DGEOM_HOST_TEST -I. test/test_vpar.c geom_fixed.c -lm -o $@

hosttest: test/test_geom_fixed test/test_vpar
	./test/test_geom_fixed
	./test/test_vpar

$(B):
	mkdir -p $(B)

# Extract just the register addresses / field offsets this firmware needs.
$(B)/generated_csr_addrs.h: $(GEN_CSR) | $(B)
	@echo "/* auto-extracted from $(GEN_CSR) -- do not edit */" > $@
	@echo "#ifndef GEOM_GENERATED_CSR_ADDRS_H" >> $@
	@echo "#define GEOM_GENERATED_CSR_ADDRS_H" >> $@
	@echo "#define CSR_BASE 0xf0000000L" >> $@
	@grep -E '#define CSR_(MAILBOX|MRDP)_' $(GEN_CSR) >> $@
	@echo "#endif" >> $@

$(B)/%.o: %.S $(B)/generated_csr_addrs.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/%.o: %.c $(B)/generated_csr_addrs.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/geom.elf: $(OBJS) geom_mips.ld $(CRT_LIB)
	$(MIPS_LD) -EL --gc-sections -Map=$(B)/geom.map -T geom_mips.ld -o $@ $(OBJS) $(CRT_LIB)
	python3 ../mips/check_isa.py --clz $(MIPS_BIN)objcopy $@ .text .text_b || { rm -f $@; false; }

$(B)/geom.bin: $(B)/geom.elf
	$(OBJCOPY) -O binary $< $@

# 32-bit-word hex, one per line -- ready for $readmemh.
$(B)/geom.init: $(B)/geom.bin
	od -An -tx4 -v $< | tr -s ' ' '\n' | sed '/^$$/d' > $@

clean:
	rm -rf build test/test_geom

.PHONY: all clean hosttest

# geom_triangle.c at -O3: its fixed-count loops (3 vertices, 3 channels) unrolled
# -- sim/geom_full -3 % (JRB) / -4 % (Bowser), +264 bytes of ROM
$(B)/geom_triangle.o: CFLAGS := $(subst -O2,-O3,$(CFLAGS))
