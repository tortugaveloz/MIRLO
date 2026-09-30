all:
include ../program.mk
CFLAGS += -O2 -Wall -fno-strict-aliasing
all: $(BUILD)/$(PROGRAM).bin
$(BUILD)/%.o: %.c
	@mkdir -p $(BUILD)
	$(MIPS_GCC) $(CFLAGS) -c $< -o $@
.SECONDARY:
