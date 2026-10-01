# A program for MIRLO's game CPU (MIPS): `make` -> build/build.bin, loaded at
# 0x4000_0000 (the Pocket's data slot 0, or tools/jtag/jtag_program_and_run.sh).
# Libraries: make -f ../../../mips/lib.mk VARIANT=game (once).
OBJECTS =  build/fungus.o
PROGRAM = build
all: build/build.bin
include ../../../mips/program.mk
CFLAGS += -O2 -Wall $(EXTRA_CFLAGS)
build/%.o: %.c
	@mkdir -p build
	$(MIPS_GCC) $(CFLAGS) -c $< -o $@
clean:
	rm -rf build
.PHONY: all clean
