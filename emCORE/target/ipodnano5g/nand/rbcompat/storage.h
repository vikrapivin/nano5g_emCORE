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
 * Adapted subset of rockbox/firmware/export/storage.h for emCORE. */
#ifndef RBCOMPAT_STORAGE_H
#define RBCOMPAT_STORAGE_H
#include "config.h"
struct storage_info
{
    unsigned int sector_size;
    sector_t num_sectors;
    char *vendor;
    char *product;
    char *revision;
};
int nand_event(long id, intptr_t data);
#endif
