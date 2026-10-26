# Copyright (C) 2026 Vikotr Krapivin
#
# This file is part of emCORE.
#
# emCORE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# emCORE is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with emCORE. If not, see <http://www.gnu.org/licenses/>.

# emCORE for the iPod nano 5G (S5L8730), standalone bundle (README.md).
#
#   make              = make n5g-image
#   make n5g-kernel   kernel: emCORE/build/ipodnano5g/emcore.bin (+ stage1.bin)
#   make n5g-image    stage 1 + kernel -> IMG1 "8730" fmt 4 (unsigned): recovery-image/emcore-nano5g.dfu
#   make n5g          clean kernel build first
#   make n5g-check    host tests of the packer (tools/test_n5g_mkimage.py)
#   make clean
#
# Switches (emCORE/target/ipodnano5g/target.mk):
#   N5G_NOCACHE=1          SDRAM non-cacheable + caches off (first-boot bisect)
#   N5G_BACKLIGHT_MAX=0x40 backlight clamp, N5G_BACKLIGHT_BOOT=0xNN boot level
#   N5G_NAND_DISABLE=1     no NAND harness (USB commands 120..131 unsupported)
#   N5G_NAND_WRITE=0       read-only harness (default 1: the Rockbox FTL writer on commands 129..131 is built in)
# Toolchains: arm-none-eabi-gcc (CROSS), a host C compiler (HOSTCC, for tools/fmssasm.c), python3 (PYTHON).
.DEFAULT_GOAL := all
CROSS  ?= arm-none-eabi-
HOSTCC ?= cc
PYTHON ?= python3
JOBS   ?= 2

N5G_BUILD := emCORE/build/ipodnano5g
N5G_DFU   := recovery-image/emcore-nano5g.dfu
N5G_RAW   := recovery-image/emcore-nano5g.bin
MKIMAGE   := tools/n5g_mkimage.py

N5G_VARS := CROSS="$(CROSS)" HOSTCC="$(HOSTCC)" PYTHON="$(PYTHON)" TARGETS=ipodnano5g
IMG1_FORMAT   ?= 4
IMG1_CERT_LEN ?= 0x300
IMG1_FLAGS := --format $(IMG1_FORMAT) --cert-len $(IMG1_CERT_LEN)
ifneq ($(N5G_NOCACHE),)
N5G_VARS += N5G_NOCACHE=1
endif
ifneq ($(N5G_BACKLIGHT_MAX),)
N5G_VARS += N5G_BACKLIGHT_MAX=$(N5G_BACKLIGHT_MAX)
endif
ifneq ($(N5G_BACKLIGHT_BOOT),)
N5G_VARS += N5G_BACKLIGHT_BOOT=$(N5G_BACKLIGHT_BOOT)
endif
ifneq ($(N5G_NAND_DISABLE),)
N5G_VARS += N5G_NAND_DISABLE=1
endif
ifneq ($(N5G_NAND_WRITE),)
N5G_VARS += N5G_NAND_WRITE=$(N5G_NAND_WRITE)
endif

.PHONY: all n5g n5g-kernel n5g-image n5g-check clean

all: n5g-image

n5g:
	+$(MAKE) -C emCORE $(N5G_VARS) clean
	+$(MAKE) -C emCORE -j$(JOBS) $(N5G_VARS) ipodnano5g

n5g-kernel:
	+$(MAKE) -C emCORE -j$(JOBS) $(N5G_VARS) ipodnano5g

n5g-image: n5g-kernel
	@mkdir -p recovery-image
	$(PYTHON) $(MKIMAGE) --stage1 $(N5G_BUILD)/stage1.bin --payload $(N5G_BUILD)/emcore.bin \
	    --stage1-elf $(N5G_BUILD)/stage1.elf --kernel-elf $(N5G_BUILD)/emcore.elf \
	    --raw $(N5G_RAW) -o $(N5G_DFU) $(IMG1_FLAGS)
	$(CROSS)nm -n $(N5G_BUILD)/emcore.elf > recovery-image/emcore-nano5g.nm

n5g-check:
	$(PYTHON) tools/test_n5g_mkimage.py

clean:
	+$(MAKE) -C emCORE $(N5G_VARS) clean
	rm -rf emCORE/build
