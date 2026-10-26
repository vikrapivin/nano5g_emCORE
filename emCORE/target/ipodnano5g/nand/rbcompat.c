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

/* GPL-2.0-or-later. Rockbox primitives the copied NAND driver calls, mapped
 * onto emCORE (compiled with the emCORE headers, unlike the driver itself). */
#include "global.h"
#include "thread.h"
#include "timer.h"
#include "mmu.h"
#include "clockgates.h"

/* the driver sees "struct mutex { void *storage[4]; }" (rbcompat/kernel.h); same object, emCORE's layout */
_Static_assert(sizeof(struct mutex) <= 4 * sizeof(void *), "rbcompat kernel.h mutex size");

void rb_mutex_init(void *m) { mutex_init((struct mutex *)m); }
void rb_mutex_lock(void *m) { mutex_lock((struct mutex *)m, TIMEOUT_BLOCK); }
void rb_mutex_unlock(void *m) { mutex_unlock((struct mutex *)m); }

uint32_t rb_usec_timer(void) { return read_usec_timer(); }
long rb_current_tick(void) { return (long)(read_usec_timer() / 10000u); }   /* HZ = 100 */

void rb_udelay(unsigned usecs) { udelay(usecs); }
void rb_sleep(int ticks) { if (ticks > 0) sleep(ticks * 10000); else yield(); }
void rb_yield(void) { yield(); }

/* ARM1176: clean (write back) / invalidate by line, as Rockbox's
 * commit_dcache_range / discard_dcache_range (mmu-armv6.S) */
void commit_dcache_range(const void *base, unsigned int size)
{
    clean_dcache_range((uint32_t)base, size);
}
void discard_dcache_range(const void *base, unsigned int size)
{
    invalidate_dcache_range((uint32_t)base, size);
}
