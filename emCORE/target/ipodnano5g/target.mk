# iPod nano 5G (S5L8730 = ARM1176JZF-S, ): kernel flags + the stage-1 SRAM loader rules.
CFLAGS_ipodnano5g += -mcpu=arm1176jzf-s

# make N5G_NOCACHE=1: SDRAM mapped Normal non-cacheable (0x4c02) by stage 1 and crt0 leaves I/D caches off.
# Bisect switch for the first boots : the default maps SDRAM write-back (0x5c06, )
# and crt0 enables both caches like the 4G.
ifneq ($(N5G_NOCACHE),)
CFLAGS_ipodnano5g += -DN5G_NOCACHE
endif
# make N5G_BACKLIGHT_MAX=0xNN: clamp applied by backlight.c (default 0x40,  step 3)
ifneq ($(N5G_BACKLIGHT_MAX),)
CFLAGS_ipodnano5g += -DN5G_BACKLIGHT_MAX=$(N5G_BACKLIGHT_MAX)
endif

# make N5G_BACKLIGHT_BOOT=0xNN: backlight level set by targetinit_late (default 0x20; 0 = stays off, still clamped above)
ifneq ($(N5G_BACKLIGHT_BOOT),)
CFLAGS_ipodnano5g += -DN5G_BACKLIGHT_BOOT=$(N5G_BACKLIGHT_BOOT)
endif

# ---- stage 1: position-dependent SRAM loader at 0x22000000 (target/ipodnano5g/stage1.S, ) ----
# Linked on its own (not part of emcore.elf); tools/n5g_mkimage.py appends emcore.bin behind stage1.bin.
N5G_STAGE1_SRC := target/ipodnano5g/stage1.S
N5G_STAGE1_LD  := target/ipodnano5g/stage1.ld

build/ipodnano5g/stage1.elf: $(N5G_STAGE1_SRC) $(N5G_STAGE1_LD)
	@echo [AS]     $<
	@mkdir -p $(dir $@)
	@$(CC) -c $(CFLAGS) $(CFLAGS_ipodnano5g) -DASM_FILE -DTARGET_ipodnano5g -Itarget/ipodnano5g -o build/ipodnano5g/stage1.o $<
	@$(LD) -T $(N5G_STAGE1_LD) -Map build/ipodnano5g/stage1.map -o $@ build/ipodnano5g/stage1.o

build/ipodnano5g/stage1.bin: build/ipodnano5g/stage1.elf
	@echo [OC]     $<
	@$(OBJCOPY) -O binary $< $@

.PHONY: n5g-stage1
n5g-stage1: build/ipodnano5g/stage1.bin

# ---- NAND: the Rockbox nano 5G driver, compiled from the copy in target/ipodnano5g/nand/ ------------------------
# (nand-nano5g.c + the shared nand/*.h engine + fmss programs + ftl-nano4g.c; the copy is the nano 5G-wip driver).
# The driver sources see only nand/rbcompat/ (Rockbox header subset); rbcompat.c and nand5g.c see emCORE's headers.
# make N5G_NAND_DISABLE=1: no NAND harness (boot bisecting; USB commands 120..131 unsupported).
# The harness exposes the driver's FTL writer (commands 129..131) by default; make N5G_NAND_WRITE=0 builds a
# read-only kernel (the capability word of command 128 then reports no writer).
N5G_NAND_WRITE ?= 1
NAND_DIR := target/ipodnano5g/nand
NAND_BUILD := build/ipodnano5g/nand
FMSSASM := build/host/fmssasm
FMSS_SRC := $(NAND_DIR)/fmss/readid.s $(NAND_DIR)/fmss/read.s $(NAND_DIR)/fmss/program.s $(NAND_DIR)/fmss/bootread.s $(NAND_DIR)/fmss/erase.s
NAND_DRIVER_SRCS := $(NAND_DIR)/nand-nano5g.c $(NAND_DIR)/ftl-nano4g.c
NAND_CFLAGS := -mcpu=arm1176jzf-s -marm -Os -ffreestanding -fno-builtin -std=gnu99 -Wall -Wextra -Wno-unused-parameter \
               -fno-pie -fno-stack-protector -fomit-frame-pointer -ffunction-sections -fdata-sections \
               -I$(NAND_DIR)/rbcompat -I$(NAND_DIR) -I$(NAND_BUILD)
ifeq ($(N5G_NAND_DISABLE),)
EXTRAOBJ_ipodnano5g := $(patsubst $(NAND_DIR)/%.c,$(NAND_BUILD)/%.o,$(NAND_DRIVER_SRCS))
CFLAGS_ipodnano5g += -I$(NAND_DIR)/emcshim -I$(NAND_DIR) -DN5G_NAND_WRITE=$(N5G_NAND_WRITE)
else
EXTRAOBJ_ipodnano5g :=
CFLAGS_ipodnano5g += -DN5G_NAND_DISABLE
PPCFLAGS += -DN5G_NAND_DISABLE
endif

$(FMSSASM): ../tools/fmssasm.c
	@echo [HOSTCC] $<
	@mkdir -p $(dir $@)
	@$(HOSTCC) -O2 -o $@ $<

$(NAND_BUILD)/fmss-nano5g.h: $(FMSS_SRC) $(FMSSASM)
	@echo [FMSSASM] $@
	@mkdir -p $(dir $@)
	@$(FMSSASM) -s $@ $(FMSS_SRC)

$(NAND_BUILD)/%.o: $(NAND_DIR)/%.c $(NAND_BUILD)/fmss-nano5g.h $(wildcard $(NAND_DIR)/*.h $(NAND_DIR)/rbcompat/*.h)
	@echo [CC]     $<
	@mkdir -p $(dir $@)
	@$(CC) -c $(NAND_CFLAGS) -o $@ $<

.PHONY: n5g-fmssasm
n5g-fmssasm: $(FMSSASM)
