/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Camera motion of the pixels the velocity prepass left untouched.
 *
 * Only moving surfaces run the velocity variant of the depth shader. Every
 * other pixel keeps the VELOCITY_INVALID clear value and gets here the motion
 * the camera alone gives it: static geometry is reprojected from its depth,
 * the background from its view direction (rotation only). Same rules as
 * EEVEE's film pass, written back in the packed format of the prepass so the
 * buffer reads the same everywhere.
 */

#include "infos/eevee_game_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_game_velocity_resolve)

#include "eevee_reverse_z_lib.glsl"
#include "eevee_velocity_lib.glsl"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int2 extent = imageSize(vector_img);
  if (any(greaterThanEqual(texel, extent))) {
    return;
  }

  /* Written by a moving surface: already object + camera motion. */
  if (imageLoad(vector_img, texel).x != VELOCITY_INVALID) {
    return;
  }

  /* The render view is the jittered one the depth was rasterized with, so the
   * reconstructed point is exact; the camera matrices are not jittered, so the
   * motion carries no jitter. */
  float depth = reverse_z::read(texelFetch(depth_tx, texel, 0).r);
  float2 uv = (float2(texel) + 0.5f) / float2(extent);
  float4 motion = velocity_resolve(float4(VELOCITY_INVALID), uv, depth);
  imageStore(vector_img, texel, velocity_pack(motion));
}
