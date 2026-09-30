# A C program for MIRLO's MIPS game CPU: include this, list OBJECTS (objects
# under $(BUILD)), and `make` gives $(BUILD)/$(PROGRAM).bin -- loaded at
# 0x4000_0000 by the Pocket (data slot 0) or the boot ROM's serial boot.
# The libraries: make -f $(MIPS_ROOT)lib.mk VARIANT=game (once).
include $(dir $(lastword $(MAKEFILE_LIST)))mips.mk
PROGRAM   ?= program
BUILD     ?= build
MIPS_LIB  := $(MIPS_ROOT)build/game
MIPS_LINKER ?= $(MIPS_ROOT)linker
MIPS_LDSCRIPT ?= $(MIPS_LINKER)/c-linker.ld
CFLAGS    += $(MIPS_GAME_CFLAGS) -I$(MIPS_INCLUDE) -isystem $(MIPS_LIB)/include -ffunction-sections -fdata-sections
CXXFLAGS  += $(CFLAGS)
MIPS_LIBS := $(MIPS_LIB)/libmirlo.a $(MIPS_LIB)/libc.a $(MIPS_LIB)/libm.a $(MIPS_LIB)/libcrt.a
MIPS_LDFLAGS += -EL -L$(MIPS_LINKER) -T $(MIPS_LDSCRIPT) --gc-sections -Map=$(BUILD)/$(PROGRAM).map

$(BUILD)/init_asm.o: $(MIPS_LINKER)/init_asm.S
	@mkdir -p $(dir $@)
	$(MIPS_GCC) $(CFLAGS) -c $< -o $@
$(BUILD)/$(PROGRAM).elf: $(BUILD)/init_asm.o $(OBJECTS) $(MIPS_LDSCRIPT)
	$(MIPS_LD) $(MIPS_LDFLAGS) -o $@ $(BUILD)/init_asm.o $(OBJECTS) --start-group $(MIPS_LIBS) --end-group
$(BUILD)/$(PROGRAM).bin: $(BUILD)/$(PROGRAM).elf
	$(MIPS_OBJCOPY) -O binary $< $@
