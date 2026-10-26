/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2008 by Marcoen Hirschberg, Bart van Adrichem
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/* Copyright (C) 2026 Vikotr Krapivin (emCORE adaptation).
 * Adapted subset of rockbox/firmware/export/s5l87xx.h for emCORE. */
/* Register subset of Rockbox's firmware/export/s5l87xx.h used by the nano 5G
 * NAND driver (values of the S5L8730 branch). */
#ifndef RBCOMPAT_S5L87XX_H
#define RBCOMPAT_S5L87XX_H
#include <stdint.h>
#define REG32_PTR_T volatile uint32_t *
#define CLK_BASE 0x3C500000
#define CLKCON1 (*((REG32_PTR_T)(CLK_BASE + 0x04)))
#define GPIO_BASE 0x3CF00000
#define GPIO_OFFSET_BITS 5
#define PCON(i) (*((REG32_PTR_T)(GPIO_BASE + ((i) << GPIO_OFFSET_BITS))))
#define PDAT(i) (*((REG32_PTR_T)(GPIO_BASE + 0x04 + ((i) << GPIO_OFFSET_BITS))))
#define GPIOCMD (*((REG32_PTR_T)(GPIO_BASE + 0x240)))
#define FMC_BASE 0x38a00000
#define FMCTRL1 (*(REG32_PTR_T)(FMC_BASE + 0x0004))
#define ECC_BASE 0x39E00000
#define ECC_CTRL (*(REG32_PTR_T)(ECC_BASE + 0x0C))
#endif
