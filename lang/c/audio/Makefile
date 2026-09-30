# Audio-core firmware: rv32i + Zmmul / ilp32 -- no FPU, no divider, no
# caches (rtl/audio/VexRiscvAudio.v, GenCoreAudio.scala). Self-contained like
# lang/c/geom: -nostdlib, so a stray float op or divide fails to LINK.
#
# Unlike the geom core's ROM, nothing here is baked into the bitstream: the
# game CPU loads build/audio_imem.bin + build/audio_dmem.bin into the core's
# IMEM/DMEM and releases its reset (lang/c/game/audio_load.c). Game firmware
# embeds them through build/audio_fw.h. So an audio firmware change needs no
# Quartus run -- rebuild this, then the game firmware that embeds it.
#
# CSR addresses (the APF audio FIFO) come from the LiteX build's csr.h.

CROSS   ?= riscv-none-elf-
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
SIZE     = $(CROSS)size

GEN_CSR  = ../../../litex/build/litex/software/include/generated/csr.h

ARCHFLAGS = -march=rv32i_zmmul_zicsr -mabi=ilp32
CFLAGS    = $(ARCHFLAGS) -O2 -g -ffreestanding -nostdlib -fno-pic -mno-relax -fsigned-char \
            -fno-strict-aliasing -Wall -Wextra -ffunction-sections -fdata-sections -std=gnu11 \
            -I$(GEN) $(CFLAGS_EXTRA)
LDFLAGS   = $(ARCHFLAGS) -nostdlib -Wl,--gc-sections -Wl,-Map,$(B)/audio.map -T audio.ld

# PROG=main (default): the bring-up program -- self-tests, benchmarks, then a
# 440 Hz tone (lang/c/game/tone_main.c loads and starts it). Another program:
# PROG=<file without .c>. Each builds into build/$(PROG)/.
PROG ?= main
B     = build/$(PROG)
GEN   = build
START = start
LINK  = $(CC) $(LDFLAGS)

# The CPU: riscv (VexRiscvAudio, the LiteX SoC) or mips (rtl/mips/mips_lite.sv,
# the MIPS SoC -- rtl/soc/mirlo_mips.sv). Same C; the CFU instructions per
# audio_cfu.h. `make CPU=mips` builds into build/mips/$(PROG)/.
ifeq ($(CPU),mips)
include ../../mips/mips.mk
B         = build/mips/$(PROG)
GEN       = build/mips
GEN_CSR   = ../../mips/include/generated/csr.h
CC        = $(MIPS_GCC)
OBJCOPY   = $(MIPS_BIN)objcopy
SIZE      = $(MIPS_BIN)size
# (no store merging, no memset/memcpy calls made of loops: -nostdlib)
ARCHFLAGS = $(MIPS_LITE_CFLAGS) -fno-store-merging -fno-tree-loop-distribute-patterns -I../mips
CFLAGS    = $(ARCHFLAGS) -O2 -g -ffreestanding -nostdlib -fsigned-char \
            -fno-strict-aliasing -Wall -Wextra -ffunction-sections -fdata-sections -std=gnu11 \
            -I$(GEN) $(CFLAGS_EXTRA)
LINK      = $(MIPS_LD) -EL --gc-sections -Map=$(B)/audio.map -T audio_mips.ld
START     = start_mips
endif
OBJS  = $(B)/$(START).o $(B)/$(PROG).o $(B)/nolibc.o

all: $(B)/audio_imem.bin $(B)/audio_dmem.bin $(B)/audio_fw.h

$(GEN) $(B):
	mkdir -p $@

$(GEN)/generated_csr_addrs.h: $(GEN_CSR) | $(GEN)
	@echo "/* auto-extracted from $(GEN_CSR) -- do not edit */" > $@
	@echo "#ifndef AUDIO_GENERATED_CSR_ADDRS_H" >> $@
	@echo "#define AUDIO_GENERATED_CSR_ADDRS_H" >> $@
	@echo "#define CSR_BASE 0xf0000000L" >> $@
	@grep -E '#define CSR_APF_AUDIO_' $(GEN_CSR) >> $@
	@grep -E '#define CONFIG_CLOCK_FREQUENCY' $(dir $(GEN_CSR))soc.h >> $@
	@echo "#endif" >> $@

$(B)/%.o: %.S | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

# IMEM is 8 KiB: the libc helpers are -Os.
$(B)/nolibc.o: CFLAGS := $(subst -O2,-Os,$(CFLAGS))
$(B)/%.o: %.c $(GEN)/generated_csr_addrs.h audio_hw.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/audio.elf: $(OBJS) audio.ld audio_mips.ld
	$(LINK) -o $@ $(OBJS)
ifeq ($(CPU),mips)
	python3 ../mips/check_isa.py $(MIPS_BIN)objcopy $@ .text || { rm -f $@; false; }
endif
	$(SIZE) $@

$(B)/audio_imem.bin: $(B)/audio.elf
	$(OBJCOPY) -O binary -j .text $< $@

$(B)/audio_dmem.bin: $(B)/audio.elf
	$(OBJCOPY) -O binary -j .rodata -j .data $< $@

# Both images as C arrays of little-endian words, for the game firmware.
$(B)/audio_fw.h: $(B)/audio_imem.bin $(B)/audio_dmem.bin
	python3 ../../../tools/bin2words.py audio_imem $(B)/audio_imem.bin audio_dmem $(B)/audio_dmem.bin > $@

clean:
	rm -rf build

.PHONY: all clean
