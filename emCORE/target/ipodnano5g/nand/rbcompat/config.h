/*
 * Copyright (C) 2026 Vikotr Krapivin
 *
 * This file is part of emCORE.
 *
 * emCORE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * emCORE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with emCORE. If not, see <http://www.gnu.org/licenses/>.
 */

/* Rockbox config subset for the nano 5G NAND driver built inside emCORE.
 * Main-firmware shape (no BOOTLOADER): the driver's write entry points are
 * compiled, the harness only references them when N5G_NAND_WRITE=1. */
#ifndef RBCOMPAT_CONFIG_H
#define RBCOMPAT_CONFIG_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define IPOD_NANO5G 1
#define S5L8730 8730
#define CONFIG_CPU S5L8730
#define CPU_ARM 1
#define ARM_ARCH 6
#define HZ 100
#define SECTOR_SIZE 4096
#define STORAGE_NAND 0x02
#define CONFIG_STORAGE STORAGE_NAND
#define CONFIG_NAND 0
#define HAVE_STORAGE_FLUSH
#define HAVE_STORAGE_READONLY
#define STORAGE_GET_INFO
#define MEMORYSIZE 64
#define IF_MD(x...)
#define IF_MD_NONVOID(x...) void
#define IF_MD_DRV(d) 0
#define CACHEALIGN_BITS 5
#define CACHEALIGN_SIZE 32
#define CACHEALIGN_ATTR __attribute__((aligned(CACHEALIGN_SIZE)))
#define ICODE_ATTR
#define INIT_ATTR
#define NORETURN_ATTR __attribute__((noreturn))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define ARRAYLEN(a) (sizeof(a) / sizeof((a)[0]))
typedef uint32_t sector_t;
#endif
