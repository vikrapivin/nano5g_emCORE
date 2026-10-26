/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Viktor Krapivin
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

/* Sectors are 4096-byte logical pages. The caller must hold nand_mutex. */
#ifndef __FTL_TARGET_H__
#define __FTL_TARGET_H__

#include <stdint.h>
#include <stdbool.h>

/* Mount the FTL. nand_phys_init() must have succeeded. Loads the block map
 * from the newest FTL context, or rebuilds it by scanning the flash if that
 * context is missing or stale (nothing is written either way). Returns 0 or
 * a negative NAND_ERR_* (nand-target.h). */
int ftl_init(void);

/* Read count logical sectors of 4096 bytes starting at sector.
 * Unwritten pages read as 0xff. Returns 0, or a negative number if a page
 * could not be read or carries the FTL's bad-data mark. */
/* (sector and count are logical 4096-byte pages, buffer receives count *
 * 4096 bytes; a CACHEALIGN_SIZE-aligned buffer lets runs of up to 16 pages
 * go through one sequencer run.) Returns -1 not mounted / out of range,
 * -2 unreadable or bad-marked page (the others are still read), -3 the
 * controller timed out. */
int ftl_read(uint32_t sector, uint32_t count, void *buffer);

#ifndef BOOTLOADER
/* Write 4096-byte sectors or publish a checkpoint. A failed mutation blocks further
 * writes and checkpoints until remount. The caller holds nand_mutex. */
int ftl_write(uint32_t sector, uint32_t count, const void *buffer);
int ftl_flush(void);
#endif

/* Number of logical pages, 0 if the FTL is not mounted */
uint32_t ftl_num_sectors(void);

/* Whether the block map came from a flash scan rather than a stored
 * context (diagnostics) */
bool ftl_was_restored(void);

#endif /* __FTL_TARGET_H__ */
