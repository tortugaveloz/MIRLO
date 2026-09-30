# The libraries of MIRLO's MIPS programs, built once per CPU variant into
# lang/mips/build/<variant>/: picolibc (include/, libc.a, libm.a -- LiteX's options),
# compiler-rt's builtins (libcrt.a: soft doubles, 64-bit division, ...) and
# mirlo_rt (libmirlo.a: the UART, the console under stdio, delays).
#
#   make -f lib.mk VARIANT=game    (the game CPU)   -> build/game/
#   make -f lib.mk VARIANT=lite    (geom and audio) -> build/lite/
# Needs meson and ninja (the LiteX venv has them).
include $(dir $(lastword $(MAKEFILE_LIST)))mips.mk
VARIANT     ?= game
VENDOR      ?= $(MIPS_ROOT)../../litex/vendor
PICOLIBC_SRC ?= $(VENDOR)/pythondata-software-picolibc/pythondata_software_picolibc/data
CRT_DIR     ?= $(VENDOR)/pythondata-software-compiler_rt/pythondata_software_compiler_rt/data/lib/builtins
MESON       ?= meson
O           := $(MIPS_ROOT)build/$(VARIANT)
ifeq ($(VARIANT),game)
ARCH_CFLAGS := $(MIPS_GAME_CFLAGS)
else
ARCH_CFLAGS := $(MIPS_LITE_CFLAGS)
endif
LIB_CFLAGS  := $(ARCH_CFLAGS) -O2 -ffunction-sections -fdata-sections

all: $(O)/libc.a $(O)/libm.a $(O)/libcrt.a $(O)/libmirlo.a

# ---- picolibc
comma := ,
$(O)/picolibc/cross.txt:
	@mkdir -p $(dir $@)
	@printf "[binaries]\nc = ['%s', '-B%s']\nar = '%sar'\nas = '%sas'\nnm = '%snm'\nstrip = '%sstrip'\n\n[host_machine]\nsystem = 'unknown'\ncpu_family = 'mips'\ncpu = 'mips'\nendian = 'little'\n\n[built-in options]\nc_args = [ %s ]\nc_link_args = [ %s ]\n" \
		"$(MIPS_CC)" "$(MIPS_TOOLS)/" "$(MIPS_BIN)" "$(MIPS_BIN)" "$(MIPS_BIN)" "$(MIPS_BIN)" \
		"$(foreach f,$(filter-out -nostdinc -isystem $(shell $(MIPS_CC) -print-file-name=include),$(LIB_CFLAGS)),'$(f)'$(comma))" \
		"'-nostdlib'" > $@
$(O)/picolibc/build.ninja: $(O)/picolibc/cross.txt
	cd $(O)/picolibc && $(MESON) setup $(abspath $(PICOLIBC_SRC)) . \
		-Dmultilib=false -Dpicocrt=false -Datomic-ungetc=false -Dthread-local-storage=false \
		-Dio-long-long=true -Dformat-default=integer -Dtests=false \
		--prefix=/ -Dincludedir=include -Dlibdir=lib \
		--cross-file cross.txt > meson.log 2>&1 || { tail -30 meson.log; false; }
$(O)/libc.a $(O)/libm.a: $(O)/picolibc/build.ninja
	cd $(O)/picolibc && $(MESON) compile > compile.log 2>&1 || { grep -m20 -B2 -A5 "error" compile.log; false; }
	rm -rf $(O)/sysroot && cd $(O)/picolibc && DESTDIR=$(abspath $(O))/sysroot $(MESON) install > install.log 2>&1
	rm -rf $(O)/include && mv $(O)/sysroot/include $(O)/include
	cp $(O)/sysroot/lib/libc.a $(O)/libc.a
	if [ -f $(O)/sysroot/lib/libm.a ]; then cp $(O)/sysroot/lib/libm.a $(O)/libm.a; else rm -f $(O)/libm.a; $(MIPS_AR) rcs $(O)/libm.a; fi

# ---- compiler-rt's builtins (gcc's limits.h ends in an #include_next of the C library's)
CRT_SRC := adddf3 subdf3 muldf3 divdf3 comparedf2 fixdfsi fixunsdfsi floatsidf floatunsidf extendsfdf2 truncdfsf2 \
           fixdfdi fixunsdfdi floatdidf floatundidf fixsfdi fixunssfdi floatdisf floatundisf negdf2 \
           divdi3 udivdi3 moddi3 umoddi3 udivmoddi4 muldi3 ashldi3 ashrdi3 lshrdi3 cmpdi2 ucmpdi2 \
           clzsi2 ctzsi2 popcountsi2 powidf2 powisf2
ifeq ($(VARIANT),lite)
CRT_SRC += addsf3 subsf3 mulsf3 divsf3 comparesf2 fixsfsi fixunssfsi floatsisf floatunsisf negsf2
endif
$(O)/crt/%.o: $(CRT_DIR)/%.c
	@mkdir -p $(O)/crt/sysinc; touch $(O)/crt/sysinc/limits.h
	@$(MIPS_GCC) $(ARCH_CFLAGS) -idirafter $(O)/crt/sysinc -O2 -fno-builtin -ffunction-sections -w -I$(CRT_DIR) -c $< -o $@
$(O)/libcrt.a: $(addprefix $(O)/crt/,$(addsuffix .o,$(CRT_SRC)))
	rm -f $@; $(MIPS_AR) rcs $@ $^

# ---- mirlo_rt
$(O)/rt/%.o: $(MIPS_ROOT)lib/%.c $(O)/libc.a
	@mkdir -p $(dir $@)
	$(MIPS_GCC) $(LIB_CFLAGS) -I$(MIPS_INCLUDE) -isystem $(O)/include -Wall -c $< -o $@
$(O)/libmirlo.a: $(O)/rt/mirlo_rt.o $(O)/rt/missing.o
	rm -f $@; $(MIPS_AR) rcs $@ $^

clean:
	rm -rf $(O)
.PHONY: all clean
