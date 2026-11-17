/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/**
 * Cube map texel directions, for the passes that write the world lighting
 * cube faces as layers.
 */

/* Direction (not normalized) through the point st in [-1, 1]^2 of a cube
 * face. s runs along the texel columns, t along the rows, as the hardware
 * selects them when sampling (OpenGL table 8.19, same in Vulkan). Faces are
 * the layers +X -X +Y -Y +Z -Z. */
float3 cube_face_direction(int face, float2 st)
{
  switch (face) {
    case 0:
      return float3(1.0f, -st.y, -st.x);
    case 1:
      return float3(-1.0f, -st.y, st.x);
    case 2:
      return float3(st.x, 1.0f, st.y);
    case 3:
      return float3(st.x, -1.0f, -st.y);
    case 4:
      return float3(st.x, -st.y, 1.0f);
    default:
      return float3(-st.x, -st.y, -1.0f);
  }
}
