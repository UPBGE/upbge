/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_common_color_utils.glsl"

void separate_hsv(float4 col, out float h, out float s, out float v)
{
  float4 hsv;

  rgb_to_hsv(col, hsv);
  h = hsv[0];
  s = hsv[1];
  v = hsv[2];
}
