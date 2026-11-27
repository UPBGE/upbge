/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Mist pass for the viewport compositor, as EEVEE's: the distance to the camera (to the view
 * plane for an orthographic one) mapped by the world mist start and depth, then shaped by the
 * falloff exponent. Written at display resolution from the render resolution depth.
 *
 *   mist_params.x = 1 / mist depth
 *   mist_params.y = -mist start / mist depth
 *   mist_params.z = falloff exponent
 *   mist_params.w = 1 for a perspective camera
 *   ray_scale     = view space x, y of the ray through NDC (1, 1), at unit depth
 *   z_planes      = clip start, clip end
 */

#include "infos/eevee_game_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_game_film_mist)

#include "eevee_reverse_z_lib.glsl"

void main()
{
  const int2 px = int2(gl_GlobalInvocationID.xy);
  const int2 size = imageSize(out_mist_img);
  if (any(greaterThanEqual(px, size))) {
    return;
  }
  const float2 uv = (float2(px) + 0.5f) / float2(size);
  const float depth = reverse_z::read(texture(depth_tx, uv).r);

  float distance;
  if (depth >= 0.9999f) {
    /* Background: at the far plane, as EEVEE reads it. */
    distance = z_planes.y;
  }
  else {
    /* Same reconstruction as the depth of field. */
    const float view_z = (z_planes.x * z_planes.y) /
                         (z_planes.y - depth * (z_planes.y - z_planes.x));
    distance = view_z;
    if (mist_params.w > 0.0f) {
      const float2 ray = (uv * 2.0f - 1.0f) * ray_scale;
      distance = view_z * sqrt(1.0f + dot(ray, ray));
    }
  }
  float mist = clamp(distance * mist_params.x + mist_params.y, 0.0f, 1.0f);
  /* Higher exponent gives a sharper transition, EEVEE's falloff shapes. */
  mist = pow(mist, mist_params.z);
  imageStore(out_mist_img, px, float4(mist));
}
