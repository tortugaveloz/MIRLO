# MIRLO's MIPS toolchain (the MIPS SoC, rtl/soc/mirlo_mips.sv): include it
# from a Makefile, then use $(MIPS_CC) with $(MIPS_GAME_CFLAGS) (the game
# CPU) or $(MIPS_LITE_CFLAGS) (the geom and audio cores).
#
#  - the game CPU, mips_core: a 32-bit VR4300 (o32: no 64-bit instructions
#    are generated), little-endian, single-precision FPU -- doubles in
#    software (compiler-rt). All 32 FPU registers hold singles: the CPU runs
#    with Status.FR 1 (lang/mips/linker/init_asm.S), where odd registers are
#    registers of their own; gas's "float register should be even" warning
#    is about FR 0, hence --no-warn;
#  - the geom and audio cores, mips_lite: MIPS32's integer subset (no FPU,
#    no madd; clz on the geom core), little-endian, soft-float.
# A GCC for MIPS (any: the flags set the byte order and the ABI) and its
# binutils: MIPS_CC / MIPS_BIN. The default is a mips-linux-gnu GCC 13.
MIPS_ROOT    := $(dir $(lastword $(MAKEFILE_LIST)))
MIPS_CC      ?= $(HOME)/local/mipsgcc/usr/bin/mips-linux-gnu-gcc-13
MIPS_BIN     ?= $(HOME)/local/mips-bin/mips-linux-gnu-
# gcc finds as/ld by their plain names in a -B directory: links to MIPS_BIN's
MIPS_TOOLS   := $(abspath $(MIPS_ROOT))/build/bin
$(shell mkdir -p $(MIPS_TOOLS) && for t in as ld ar nm objcopy strip; do ln -sf $(MIPS_BIN)$$t $(MIPS_TOOLS)/$$t; done)
MIPS_GCC      = $(MIPS_CC) -B$(MIPS_TOOLS)/
MIPS_AR       = $(MIPS_BIN)ar
MIPS_OBJCOPY  = $(MIPS_BIN)objcopy -I elf32-tradlittlemips
MIPS_NM       = $(MIPS_BIN)nm
MIPS_LD       = $(MIPS_BIN)ld

MIPS_COMMON  := -mabi=32 -EL -mno-abicalls -fno-pic -G0 -nostdinc -isystem $(shell $(MIPS_CC) -print-file-name=include) \
                -ffreestanding -fsigned-char -ffp-contract=off
MIPS_GAME_ARCH := -march=vr4300 -mhard-float -msingle-float -mfp32 -Wa,--no-warn
MIPS_LITE_ARCH := -march=mips32 -msoft-float -mno-imadd -mno-check-zero-division
MIPS_GAME_CFLAGS = $(MIPS_COMMON) $(MIPS_GAME_ARCH)
MIPS_LITE_CFLAGS = $(MIPS_COMMON) $(MIPS_LITE_ARCH)
MIPS_INCLUDE := $(MIPS_ROOT)include
