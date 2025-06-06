/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

void normal_new_shading(float3 nor, float3 dir, out float3 outnor, out float outdot)
{
  outnor = dir;
  outdot = dot(normalize(nor), dir);
}
