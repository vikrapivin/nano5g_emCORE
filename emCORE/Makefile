NAME := emcore
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

ifeq ($(shell uname),WindowsNT)
CCACHE :=
else
CCACHE := $(shell which ccache)
endif

CROSS   ?= arm-none-eabi-
PYTHON  ?= python3
CC      := $(CCACHE) $(CROSS)gcc
AS      := $(CROSS)as
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
HOSTCC  ?= cc
UCLPACK := false   # the nano 5G image is packaged by ../tools/n5g_mkimage.py, no UCL

CFLAGS   += -Os -fno-pie -fno-stack-protector -fomit-frame-pointer -I. -Iexport -ffunction-sections -fdata-sections -marm
LDFLAGS  += "$(shell $(CC) -print-libgcc-file-name)" --gc-sections
PPCFLAGS += -DASM_FILE

preprocess = $(shell $(CC) $(PPCFLAGS) $(2) -E -P -x c $(1) | grep -v "^\#")
preprocesspaths = $(shell $(CC) $(PPCFLAGS) $(2) -E -P -x c $(1) | grep -v "^\#" | sed -e "s:^..*:$(dir $(1))&:" | sed -e "s:^\\./::")

# TODO: extract revision from Git
REVISION := 977
REVISIONINT := $(shell echo $(REVISION) | sed -e "s/[^0-9].*$$//")

TARGETS := $(call preprocess,TARGETS,-I.)

define TARGET_template
-include target/$(1)/target.mk

SRC_$(1) := $$(call preprocesspaths,SOURCES,-DTARGET_$(1) -DTARGET=\"$(1)\" -DCONFIG_H=\"target/$(1)/config.h\" -DTARGET_H=\"target/$(1)/target.h\" -D_NO_VERSION_H_ -Itarget/$(1) -I.)
OBJ_$(1) := $$(SRC_$(1):%.c=build/$(1)/%.o)
OBJ_$(1) := $$(OBJ_$(1):%.S=build/$(1)/%.o)
# objects built by target.mk rules outside SOURCES (the Rockbox NAND driver copy in target/ipodnano5g/nand)
OBJ_$(1) += $$(EXTRAOBJ_$(1))

-include $$(OBJ_$(1):%=%.dep)

ifeq ($(1),ipodnano5g)
$$(OBJ_$(1)): build/$(1)/nand-config
# nano 5G: the raw emcore.bin is packaged by tools/n5g_mkimage.py (stage 1 + IMG1), no UCL (top-level make n5g-image)
$(1): build/$(1)/$(NAME).bin build/$(1)/stage1.bin
else
$(1): build/$(1)/$(NAME)-$(1).ucl
endif

build/$(1)/$(NAME)-$(1).ucl: build/$(1)/$(NAME).bin
	@echo [UCL]    $$<
	@$(UCLPACK) $$^ $$@

build/$(1)/$(NAME).bin: build/$(1)/$(NAME).elf
	@echo [OC]     $$<
	@$(OBJCOPY) -O binary $$^ $$@

build/$(1)/$(NAME).elf: target/$(1)/ls.x build/$(1)/target/$(1)/crt0.o $$(OBJ_$(1))
	@echo [LD]     $$@
	@$(LD) $(LDFLAGS) $$(LDFLAGS_$(1)) -o $$@ -T target/$(1)/ls.x $$(OBJ_$(1))

build/$(1)/%.o: %.c build/version.h
	@echo [CC]     $$<
ifeq ($(shell uname),WindowsNT)
	@-if not exist $$(subst /,\,$$(dir $$@)) md $$(subst /,\,$$(dir $$@))
else
	@-mkdir -p $$(dir $$@)
endif
	@$(CC) -c $(CFLAGS) $$(CFLAGS_$(1)) -DTARGET_$(1) -DTARGET=\"$(1)\" -DCONFIG_H=\"target/$(1)/config.h\" -DTARGET_H=\"target/$(1)/target.h\" -o $$@ $$<
	@$(CC) -MM $(CFLAGS) $$(CFLAGS_$(1)) -DTARGET_$(1) -DTARGET=\"$(1)\" -DCONFIG_H=\"target/$(1)/config.h\" -DTARGET_H=\"target/$(1)/target.h\" $$< > $$@.dep.tmp
	@sed -e "s|.*:|$$@:|" < $$@.dep.tmp > $$@.dep
ifeq ($(shell uname),WindowsNT)
	@sed -e "s/.*://" -e "s/\\$$$$//" < $$@.dep.tmp | fmt -1 | sed -e "s/^ *//" -e "s/$$$$/:/" >> $$@.dep
else
	@sed -e 's/.*://' -e 's/\\$$$$//' < $$@.dep.tmp | fmt -1 | sed -e 's/^ *//' -e 's/$$$$/:/' >> $$@.dep
endif
	@rm -f $$@.dep.tmp

build/$(1)/%.o: %.S build/version.h
	@echo [CC]     $$<
ifeq ($(shell uname),WindowsNT)
	@-if not exist $$(subst /,\,$$(dir $$@)) md $$(subst /,\,$$(dir $$@))
else
	@-mkdir -p $$(dir $$@)
endif
	@$(CC) -c $(CFLAGS) $$(CFLAGS_$(1)) -DTARGET_$(1) -DTARGET=\"$(1)\" -DCONFIG_H=\"target/$(1)/config.h\" -DTARGET_H=\"target/$(1)/target.h\" -o $$@ $$<
	@$(CC) -MM $(CFLAGS) $$(CFLAGS_$(1)) -DTARGET_$(1) -DTARGET=\"$(1)\" -DCONFIG_H=\"target/$(1)/config.h\" -DTARGET_H=\"target/$(1)/target.h\" $$< > $$@.dep.tmp
	@sed -e "s|.*:|$$@:|" < $$@.dep.tmp > $$@.dep
ifeq ($(shell uname),WindowsNT)
	@sed -e "s/.*://" -e "s/\\$$$$//" < $$@.dep.tmp | fmt -1 | sed -e "s/^ *//" -e "s/$$$$/:/" >> $$@.dep
else
	@sed -e 's/.*://' -e 's/\\$$$$//' < $$@.dep.tmp | fmt -1 | sed -e 's/^ *//' -e 's/$$$$/:/' >> $$@.dep
endif
	@rm -f $$@.dep.tmp
endef

all: $(TARGETS)

$(foreach target,$(TARGETS),$(eval $(call TARGET_template,$(target))))

build/version.h: version.h
	@echo [PP]     $<
ifeq ($(shell uname),WindowsNT)
	@-if not exist build md build
	@sed -e "s/\$$REVISION\$$/$(REVISION)/" -e "s/\$$REVISIONINT\$$/$(REVISIONINT)/" < $< > $@
else
	@-mkdir -p build
	@sed -e 's/\$$REVISION\$$/$(REVISION)/' -e 's/\$$REVISIONINT\$$/$(REVISIONINT)/' < $< > $@
endif

clean:
	@rm -rf build

.PHONY: all clean $(TARGETS)

.PHONY: n5g-config-force
build/ipodnano5g/nand-config: n5g-config-force
	@mkdir -p $(dir $@)
	@echo 'write=$(N5G_NAND_WRITE) disable=$(N5G_NAND_DISABLE)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@
