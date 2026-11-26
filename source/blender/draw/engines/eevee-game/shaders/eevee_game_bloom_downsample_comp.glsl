/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Bloom downsample, as legacy EEVEE.
 *
 * The blit reads the scene color, keeps the part above the threshold with a quadratic knee and
 * clamps it. The other levels halve the previous one.
 *
 * curve_threshold: (threshold - knee, 2 knee, 0.25 / knee, threshold).
 *
 * Local group 8x8.
 */

#include "infos/eevee_game_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_game_bloom_downsample)

#include "eevee_game_bloom_lib.glsl"

void main()
{
  const int2 out_px = int2(gl_GlobalInvocationID.xy);
  const int2 out_res = imageSize(out_color_img);
  if (any(greaterThanEqual(out_px, out_res))) {
    return;
  }
  const float2 uv = (float2(out_px) + 0.5f) / float2(out_res);
  const float2 texel = 1.0f / float2(textureSize(in_color_tx, 0));

  float3 color;
  if (is_blit) {
    color = bloom_downsample_antiflicker(in_color_tx, uv, texel);
    float br = bloom_brightness(color);
    /* Under the threshold: quadratic curve over the knee. */
    float rq = clamp(br - curve_threshold.x, 0.0f, curve_threshold.y);
    rq = curve_threshold.z * rq * rq;
    color *= max(rq, br - curve_threshold.w) / max(1e-5f, br);
    if (clamp_intensity > 0.0f) {
      br = max(1e-5f, bloom_brightness(color));
      color *= 1.0f - max(0.0f, br - clamp_intensity) / br;
    }
    /* A NaN or an infinite value in the scene would spread over the whole bloom. */
    if (!all(lessThan(abs(color), float3(65000.0f)))) {
      color = float3(0.0f);
    }
  }
  else {
    color = bloom_downsample(in_color_tx, uv, texel);
  }
  imageStore(out_color_img, out_px, float4(color, 1.0f));
}
