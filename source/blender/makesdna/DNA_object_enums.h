/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 *
 * Enums typedef's for use in public headers.
 */

#pragma once

/** #Object.mode */
typedef enum eObjectMode {
  OB_MODE_OBJECT = 0,
  OB_MODE_EDIT = 1 << 0,
  OB_MODE_SCULPT = 1 << 1,
  OB_MODE_VERTEX_PAINT = 1 << 2,
  OB_MODE_WEIGHT_PAINT = 1 << 3,
  OB_MODE_TEXTURE_PAINT = 1 << 4,
  OB_MODE_PARTICLE_EDIT = 1 << 5,
  OB_MODE_POSE = 1 << 6,
  OB_MODE_EDIT_GPENCIL_LEGACY = 1 << 7,
  OB_MODE_PAINT_GREASE_PENCIL = 1 << 8,
  OB_MODE_SCULPT_GREASE_PENCIL = 1 << 9,
  OB_MODE_WEIGHT_GREASE_PENCIL = 1 << 10,
  OB_MODE_VERTEX_GREASE_PENCIL = 1 << 11,
  OB_MODE_SCULPT_CURVES = 1 << 12,
} eObjectMode;

/** #Object.dt, #View3DShading.type */
typedef enum eDrawType {
  OB_BOUNDBOX = 1,
  OB_WIRE = 2,
  OB_SOLID = 3,
  OB_MATERIAL = 4,
  OB_TEXTURE = 5,
  OB_RENDER = 6,
} eDrawType;

/** Any mode where the brush system is used. */
#define OB_MODE_ALL_PAINT \
  (OB_MODE_SCULPT | OB_MODE_VERTEX_PAINT | OB_MODE_WEIGHT_PAINT | OB_MODE_TEXTURE_PAINT)

#define OB_MODE_ALL_PAINT_GPENCIL \
  (OB_MODE_PAINT_GREASE_PENCIL | OB_MODE_SCULPT_GREASE_PENCIL | OB_MODE_WEIGHT_GREASE_PENCIL | \
   OB_MODE_VERTEX_GREASE_PENCIL)

/** Any mode that uses Object.sculpt. */
#define OB_MODE_ALL_SCULPT (OB_MODE_SCULPT | OB_MODE_VERTEX_PAINT | OB_MODE_WEIGHT_PAINT)

/** Any mode that uses weight-paint. */
#define OB_MODE_ALL_WEIGHT_PAINT (OB_MODE_WEIGHT_PAINT | OB_MODE_WEIGHT_GREASE_PENCIL)

/**
 * Any mode that has data or for Grease Pencil modes, we need to free when switching modes,
 * see: #blender::ed::object::mode_generic_exit
 */
#define OB_MODE_ALL_MODE_DATA \
  (OB_MODE_EDIT | OB_MODE_VERTEX_PAINT | OB_MODE_WEIGHT_PAINT | OB_MODE_SCULPT | OB_MODE_POSE | \
   OB_MODE_PAINT_GREASE_PENCIL | OB_MODE_EDIT_GPENCIL_LEGACY | OB_MODE_SCULPT_GREASE_PENCIL | \
   OB_MODE_WEIGHT_GREASE_PENCIL | OB_MODE_VERTEX_GREASE_PENCIL | OB_MODE_SCULPT_CURVES)
