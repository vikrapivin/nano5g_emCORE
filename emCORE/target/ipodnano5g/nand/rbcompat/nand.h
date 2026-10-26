/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Alan Korr
 * Copyright (C) 2008 by Frank Gevaerts
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
 * Adapted subset of rockbox/firmware/export/nand.h for emCORE. */
#ifndef RBCOMPAT_NAND_H
#define RBCOMPAT_NAND_H
#include "config.h"
#include "storage.h"
int nand_init(void);
void nand_spindown(int seconds);
void nand_spin(void);
void nand_sleepnow(void);
int nand_flush(void);
bool nand_readonly(void);
int nand_read_sectors(sector_t start, int incount, void *inbuf);
int nand_write_sectors(sector_t start, int count, const void *outbuf);
void nand_get_info(struct storage_info *info);
long nand_last_disk_activity(void);
#endif
