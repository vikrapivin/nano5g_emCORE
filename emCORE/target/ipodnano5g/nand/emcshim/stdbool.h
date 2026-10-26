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

/* see stdint.h; emCORE's global.h already defines bool */
#ifndef EMCSHIM_STDBOOL_H
#define EMCSHIM_STDBOOL_H
#ifndef __cplusplus
#ifndef bool
#define bool _Bool
#endif
#ifndef true
#define true 1
#define false 0
#endif
#endif
#endif
