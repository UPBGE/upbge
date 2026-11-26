/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Bloom upsample, as legacy EEVEE: the tent filtered coarser level is added to the finer one,
 * in place. The finer level is only read here, at the same texel.
 *
 * Local group 8x8.
 */

#include "infos/eevee_game_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_game_bloom_upsample)

#include "eevee_game_bloom_lib.glsl"

void main()
{
  const int2 out_px = int2(gl_GlobalInvocationID.xy);
  const int2 out_res = imageSize(out_color_img);
  if (any(greaterThanEqual(out_px, out_res))) {
    return;
  }
  const float2 uv = (float2(out_px) + 0.5f) / float2(out_res);
  const float2 texel = 1.0f / float2(textureSize(in_blur_tx, 0));
  const float3 blur = bloom_upsample(in_blur_tx, uv, texel, sample_scale);
  const float4 base = imageLoad(out_color_img, out_px);
  imageStore(out_color_img, out_px, float4(base.rgb + blur, base.a));
}
