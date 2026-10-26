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

#ifndef RBCOMPAT_SYSTEM_H
#define RBCOMPAT_SYSTEM_H
#include "config.h"
#include "s5l87xx.h"
/* rbcompat.c */
void rb_udelay(unsigned usecs);
#define udelay(us) rb_udelay(us)
uint32_t rb_usec_timer(void);
void commit_dcache_range(const void *base, unsigned int size);
void discard_dcache_range(const void *base, unsigned int size);
#define USEC_TIMER (rb_usec_timer())
#endif
