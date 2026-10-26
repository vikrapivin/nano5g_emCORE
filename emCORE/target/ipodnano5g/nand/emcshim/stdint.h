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

/* emCORE-side glue (nand5g.c, usb/usbdebug.c) includes the Rockbox driver's public headers (nand/nand-target.h,
 * nand/ftl-target.h) and nand5g.h, which use <stdint.h>. Resolve that to emCORE's own libc stdint.h (macro types)
 * instead of the toolchain's typedefs, which would collide with emCORE's #defines. Same widths on ARM (uint32_t =
 * unsigned long in both), so the ABI with the separately compiled driver objects (toolchain stdint) is identical.
 * The driver objects never see this directory. */
#include "libc/include/stdint.h"
