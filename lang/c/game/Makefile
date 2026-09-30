# Game-CPU programs that draw through the geometry core and MRDP: frame.c
# (framebuffers, GDL submission, flips) + one program. Standard LiteX C build
# (rv32imafc / ilp32f), same pattern as lang/c/examples/*.
#   make                  the spinning cube (cube_main.c)
#   make PROG=demo        the spinning triangle (demo_main.c)
#   make PROG=videotest   the scan-out test (videotest_main.c)
#   make PROG=tone        the audio core bring-up + a 440 Hz tone (tone_main.c)
#   make CPU=mips ...     the same for the MIPS SoC (rtl/soc/mirlo_mips.sv) -> build/mips/build.bin

PROG ?= cube
OBJECTS_tone = audio_load.o

ifeq ($(CPU),mips)
# ---- the MIPS SoC (rtl/soc/mirlo_mips.sv): lang/mips's toolchain and libraries
BUILD    = build/mips
PROGRAM  = build
OBJECTS  = $(addprefix $(BUILD)/,log.o frame.o $(PROG)_main.o $(OBJECTS_$(PROG)))
all: $(BUILD)/build.bin
include ../../mips/program.mk
CFLAGS  += -O2 -Wall -fno-strict-aliasing -I. -I../geom $(EXTRA_CFLAGS) -DAUDIO_FW_HEADER='"../audio/build/mips/main/audio_fw.h"'
GEOM_ELF ?= ../geom/build/mips/geom.elf
GEOM_TRIS_EMITTED_ADDR := $(shell $(MIPS_NM) $(GEOM_ELF) 2>/dev/null | sed -n 's/^\([0-9a-f]*\) [BbDd] geom_tris_emitted$$/0x\1u/p')
ifeq ($(GEOM_TRIS_EMITTED_ADDR),)
$(error cannot find geom_tris_emitted in $(GEOM_ELF) -- build lang/c/geom (CPU=mips) first)
endif
CFLAGS  += -DGEOM_TRIS_EMITTED_ADDR=$(GEOM_TRIS_EMITTED_ADDR)
$(BUILD)/%.o: %.c
	@mkdir -p $(BUILD)
	$(MIPS_GCC) $(CFLAGS) -c $< -o $@
clean:
	rm -rf build
.PHONY: all clean
else
LITEX_ROOT_DIRECTORY = ../../../litex
OUTPUT_DIRECTORY = ./build

OBJECTS = init_asm.o log.o frame.o $(PROG)_main.o $(OBJECTS_$(PROG))

BUILD_DIR = $(LITEX_ROOT_DIRECTORY)/build/litex
SOC_DIRECTORY = $(LITEX_ROOT_DIRECTORY)/vendor/litex/litex/soc
LINKER_DIRECTORY = ../../linker
BASE_LINKER_FILE = $(LINKER_DIRECTORY)/c-linker.ld

include $(BUILD_DIR)/software/include/generated/variables.mak
include $(SOC_DIRECTORY)/software/common.mak

# Extra defines for diagnostic builds, e.g. EXTRA_CFLAGS="-DDBG_FRAMES=40 -DDBG_SYNC"
CFLAGS += $(EXTRA_CFLAGS)

# geom_tris_emitted lives at a LINKED address in geom RAM that moves whenever
# the geom firmware changes: take it from the geom.elf baked into the
# bitstream -- by default the current geom build; point GEOM_ELF at another
# copy for another bitstream. `make clean` after changing it (CFLAGS are not
# a dependency of the objects).
GEOM_ELF ?= ../geom/build/geom.elf
GEOM_TRIS_EMITTED_ADDR := $(shell $(TRIPLE)-nm $(GEOM_ELF) 2>/dev/null | sed -n 's/^\([0-9a-f]*\) [BbDd] geom_tris_emitted$$/0x\1u/p')
ifeq ($(GEOM_TRIS_EMITTED_ADDR),)
$(error cannot find geom_tris_emitted in $(GEOM_ELF) -- build lang/c/geom first)
endif
CFLAGS += -DGEOM_TRIS_EMITTED_ADDR=$(GEOM_TRIS_EMITTED_ADDR)

all: build.bin

%.bin: %.elf
	$(OBJCOPY) -O binary $(OUTPUT_DIRECTORY)/$< $(OUTPUT_DIRECTORY)/$@
ifneq ($(OS),Windows_NT)
	chmod -x $(OUTPUT_DIRECTORY)/$@
endif

vpath %.a $(PACKAGES:%=../%)

build.elf: $(OBJECTS:%=$(OUTPUT_DIRECTORY)/%)
	$(CC) $(LDFLAGS) -L $(LINKER_DIRECTORY) -T $(BASE_LINKER_FILE) -N -o $(OUTPUT_DIRECTORY)/$@ \
		$(OBJECTS:%=$(OUTPUT_DIRECTORY)/%) \
		$(PACKAGES:%=-L$(BUILD_DIR)/software/%) \
		-Wl,--whole-archive -Wl,--gc-sections -Wl,-Map,$(OUTPUT_DIRECTORY)/$@.map \
		$(LIBS:lib%=-l%)
ifneq ($(OS),Windows_NT)
	chmod -x $(OUTPUT_DIRECTORY)/$@
endif

-include $(OBJECTS:%.o=$(OUTPUT_DIRECTORY)/%.d)

VPATH = $(BIOS_DIRECTORY):$(BIOS_DIRECTORY)/cmds:$(CPU_DIRECTORY):$(LINKER_DIRECTORY)

$(OUTPUT_DIRECTORY)/%.o: %.c
	mkdir -p $(OUTPUT_DIRECTORY)
	$(compile)

$(OUTPUT_DIRECTORY)/%.o: %.S
	mkdir -p $(OUTPUT_DIRECTORY)
	$(assemble)

clean:
	$(RM) -R $(OUTPUT_DIRECTORY)

.PHONY: all clean hosttest
endif
