# Tests for MIRLO's MIPS SoC, run in sim/mips_soc (tb_soc build/<test>.bin).
TESTS := cpu_test libc_test
all: $(TESTS)
$(TESTS):
	$(MAKE) -f test.mk PROGRAM=$@ OBJECTS=build/$@.o
clean:
	rm -rf build
.PHONY: all clean $(TESTS)
