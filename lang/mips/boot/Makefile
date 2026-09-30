# MIRLO's boot ROM on the MIPS SoC (4 KiB at 0xBFC0_0000) -> build/boot.hex,
# one 32-bit word a line: tools/mips_inits.py puts it in the bitstream.
include ../mips.mk
B       := build
CFLAGS  := $(MIPS_GAME_CFLAGS) -Os -I$(MIPS_INCLUDE) -Wall -fno-builtin -ffunction-sections
all: $(B)/boot.hex
$(B):
	mkdir -p $@
$(B)/%.o: %.S | $(B)
	$(MIPS_GCC) $(CFLAGS) -c $< -o $@
$(B)/%.o: %.c | $(B)
	$(MIPS_GCC) $(CFLAGS) -c $< -o $@
$(B)/boot.elf: $(B)/start.o $(B)/boot.o boot.ld
	$(MIPS_LD) -EL -T boot.ld --gc-sections -Map=$(B)/boot.map -o $@ $(B)/start.o $(B)/boot.o
$(B)/boot.bin: $(B)/boot.elf
	$(MIPS_OBJCOPY) -O binary $< $@
$(B)/boot.hex: $(B)/boot.bin
	od -An -tx4 -v $< | tr -s ' ' '\n' | sed '/^$$/d' > $@
	@echo "boot ROM: $$(stat -c %s $<) of 4096 bytes"
clean:
	rm -rf $(B)
.PHONY: all clean
