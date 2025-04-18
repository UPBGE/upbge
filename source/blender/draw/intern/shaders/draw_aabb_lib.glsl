/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "draw_shape_lib.glsl"

/* ---------------------------------------------------------------------- */
/** \name Axis Aligned Bound Box
 * \{ */

struct AABB {
  float3 min, max;
};

AABB shape_aabb(float3 min, float3 max)
{
  AABB aabb;
  aabb.min = min;
  aabb.max = max;
  return aabb;
}

AABB aabb_init_min_max()
{
  AABB aabb;
  aabb.min = float3(1.0e30f);
  aabb.max = float3(-1.0e30f);
  return aabb;
}

void aabb_merge(inout AABB aabb, float3 v)
{
  aabb.min = min(aabb.min, v);
  aabb.max = max(aabb.max, v);
}

/**
 * Return true if there is any intersection.
 */
bool aabb_intersect(AABB a, AABB b)
{
  return all(greaterThanEqual(min(a.max, b.max), max(a.min, b.min)));
}

/**
 * Compute intersect intersection volume of \a a and \a b.
 * Return true if the resulting volume is not empty.
 */
bool aabb_clip(AABB a, AABB b, out AABB c)
{
  c.min = max(a.min, b.min);
  c.max = min(a.max, b.max);
  return all(greaterThanEqual(c.max, c.min));
}

Box aabb_to_box(AABB aabb)
{
  Box box;
  box.corners[0] = aabb.min;
  box.corners[1] = float3(aabb.max.x, aabb.min.y, aabb.min.z);
  box.corners[2] = float3(aabb.max.x, aabb.max.y, aabb.min.z);
  box.corners[3] = float3(aabb.min.x, aabb.max.y, aabb.min.z);
  box.corners[4] = float3(aabb.min.x, aabb.min.y, aabb.max.z);
  box.corners[5] = float3(aabb.max.x, aabb.min.y, aabb.max.z);
  box.corners[6] = aabb.max;
  box.corners[7] = float3(aabb.min.x, aabb.max.y, aabb.max.z);
  return box;
}

/** \} */
