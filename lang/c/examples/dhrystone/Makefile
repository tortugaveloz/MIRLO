# A program for MIRLO's game CPU (MIPS): `make` -> build/build.bin, loaded at
# 0x4000_0000 (the Pocket's data slot 0, or tools/jtag/jtag_program_and_run.sh).
# Libraries: make -f ../../../mips/lib.mk VARIANT=game (once).
OBJECTS =  build/dhry_1.o build/dhry_2.o build/time.o
PROGRAM = build
all: build/build.bin
include ../../../mips/program.mk
# (the benchmark's own flags: -fno-inline as VexRiscv's methodology; old K&R C)
CFLAGS += -O3 -fno-inline -DTIME -DDHRY_ITERS=20000000 -fno-builtin-printf -fno-common -falign-functions=4 -w $(EXTRA_CFLAGS)
build/%.o: %.c
	@mkdir -p build
	$(MIPS_GCC) $(CFLAGS) -c $< -o $@
clean:
	rm -rf build
.PHONY: all clean
