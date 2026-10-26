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

#ifndef RBCOMPAT_KERNEL_H
#define RBCOMPAT_KERNEL_H
#include "config.h"
/* The emCORE mutex (thread.h: owner, waiters, owned_next, count) behind a
 * Rockbox-shaped API; rbcompat.c checks the size. */
struct mutex { void *storage[4]; };
void rb_mutex_init(struct mutex *m);
void rb_mutex_lock(struct mutex *m);
void rb_mutex_unlock(struct mutex *m);
#define mutex_init(m)   rb_mutex_init(m)
#define mutex_lock(m)   rb_mutex_lock(m)
#define mutex_unlock(m) rb_mutex_unlock(m)
long rb_current_tick(void);
#define current_tick (rb_current_tick())
#define TIME_AFTER(a, b) ((long)(b) - (long)(a) < 0)
#define TIME_BEFORE(a, b) TIME_AFTER(b, a)
void rb_sleep(int ticks);
void rb_yield(void);
#define sleep(t) rb_sleep(t)
#define yield()  rb_yield()
#endif
