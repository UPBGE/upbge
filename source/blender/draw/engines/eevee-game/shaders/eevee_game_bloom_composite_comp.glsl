/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Bloom resolve, as legacy EEVEE: the finest accumulated level, tent filtered, times the bloom
 * color (color times intensity) is added to the scene color in place.
 *
 * Local group 8x8.
 */

#include "infos/eevee_game_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_game_bloom_composite)

#include "eevee_game_bloom_lib.glsl"

void main()
{
  const int2 out_px = int2(gl_GlobalInvocationID.xy);
  const int2 out_res = imageSize(combined_img);
  if (any(greaterThanEqual(out_px, out_res))) {
    return;
  }
  const float2 uv = (float2(out_px) + 0.5f) / float2(out_res);
  const float2 texel = 1.0f / float2(textureSize(bloom_tx, 0));
  const float3 blur = bloom_upsample(bloom_tx, uv, texel, sample_scale);
  const float4 combined = imageLoad(combined_img, out_px);
  imageStore(combined_img, out_px, float4(combined.rgb + blur * bloom_color, combined.a));
}
