/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** ile
 * Work-group sizes of the FSR 3.1 upscaler passes. A header of its own: the create infos only
 * keep the includes of `*defines.hh` headers when they are compiled as C++.
 */

#pragma once

/* Work-group side of every pass but RCAS and the two SPD pyramids. */
#define FSR3_GROUP_SIZE 8
/* RCAS: 64 threads per group, 4 pixels per thread over a 16x16 tile. */
#define FSR3_RCAS_GROUP_SIZE 64
#define FSR3_RCAS_TILE_SIZE 16
/* SPD: 256 threads per group, 64x64 source texels per group. */
#define FSR3_SPD_GROUP_SIZE 256
