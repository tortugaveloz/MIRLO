# Game-CPU programs that draw through the geometry core and MRDP: frame.c
# (framebuffers, GDL submission, flips) + one program, for the MIPS game CPU
# (lang/mips/program.mk; libraries: make -f ../../mips/lib.mk VARIANT=game).
#   make                  the spinning cube (cube_main.c)
#   make PROG=demo        the spinning triangle (demo_main.c)
#   make PROG=videotest   the scan-out test (videotest_main.c)
#   make PROG=tone        the audio core bring-up + a 440 Hz tone (tone_main.c)

PROG ?= cube
OBJECTS_tone = audio_load.o

# lang/mips: the toolchain, picolibc, compiler-rt, crt0 and linker files
BUILD    = build
PROGRAM  = build
OBJECTS  = $(addprefix $(BUILD)/,log.o frame.o $(PROG)_main.o $(OBJECTS_$(PROG)))
all: $(BUILD)/build.bin
include ../../mips/program.mk
CFLAGS  += -O2 -Wall -fno-strict-aliasing -I. -I../geom $(EXTRA_CFLAGS) -DAUDIO_FW_HEADER='"../audio/build/main/audio_fw.h"'
GEOM_ELF ?= ../geom/build/geom.elf
GEOM_TRIS_EMITTED_ADDR := $(shell $(MIPS_NM) $(GEOM_ELF) 2>/dev/null | sed -n 's/^\([0-9a-f]*\) [BbDd] geom_tris_emitted$$/0x\1u/p')
ifeq ($(GEOM_TRIS_EMITTED_ADDR),)
$(error cannot find geom_tris_emitted in $(GEOM_ELF) -- build lang/c/geom first)
endif
CFLAGS  += -DGEOM_TRIS_EMITTED_ADDR=$(GEOM_TRIS_EMITTED_ADDR)
$(BUILD)/%.o: %.c
	@mkdir -p $(BUILD)
	$(MIPS_GCC) $(CFLAGS) -c $< -o $@
clean:
	rm -rf build
.PHONY: all clean
