/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/**
 * Filters of the legacy EEVEE bloom (effect_bloom_frag.glsl, Blender 2.80 to 4.1).
 * `texel` is the texel size of the sampled texture, sampled with bilinear filtering.
 */

float bloom_brightness(float3 c)
{
  return max(max(c.r, c.g), c.b);
}

/* 4x4 box: four bilinear taps on the texel corners. */
float3 bloom_downsample(sampler2D tex, float2 uv, float2 texel)
{
  float4 d = texel.xyxy * float4(-1.0f, -1.0f, 1.0f, 1.0f);
  float3 s = textureLod(tex, uv + d.xy, 0.0f).rgb;
  s += textureLod(tex, uv + d.zy, 0.0f).rgb;
  s += textureLod(tex, uv + d.xw, 0.0f).rgb;
  s += textureLod(tex, uv + d.zw, 0.0f).rgb;
  return s * 0.25f;
}

/* Same footprint, weighted by 1 / (1 + brightness) (Karis 2013): an isolated very bright pixel
 * moving by a texel does not make the whole bloom flicker. */
float3 bloom_downsample_antiflicker(sampler2D tex, float2 uv, float2 texel)
{
  float4 d = texel.xyxy * float4(-1.0f, -1.0f, 1.0f, 1.0f);
  float3 s1 = textureLod(tex, uv + d.xy, 0.0f).rgb;
  float3 s2 = textureLod(tex, uv + d.zy, 0.0f).rgb;
  float3 s3 = textureLod(tex, uv + d.xw, 0.0f).rgb;
  float3 s4 = textureLod(tex, uv + d.zw, 0.0f).rgb;
  float w1 = 1.0f / (bloom_brightness(s1) + 1.0f);
  float w2 = 1.0f / (bloom_brightness(s2) + 1.0f);
  float w3 = 1.0f / (bloom_brightness(s3) + 1.0f);
  float w4 = 1.0f / (bloom_brightness(s4) + 1.0f);
  return (s1 * w1 + s2 * w2 + s3 * w3 + s4 * w4) / (w1 + w2 + w3 + w4);
}

/* 9 tap tent (bilinear taps), `sample_scale` texels apart. */
float3 bloom_upsample(sampler2D tex, float2 uv, float2 texel, float sample_scale)
{
  float4 d = texel.xyxy * float4(1.0f, 1.0f, -1.0f, 0.0f) * sample_scale;
  float3 s = textureLod(tex, uv - d.xy, 0.0f).rgb;
  s += textureLod(tex, uv - d.wy, 0.0f).rgb * 2.0f;
  s += textureLod(tex, uv - d.zy, 0.0f).rgb;
  s += textureLod(tex, uv + d.zw, 0.0f).rgb * 2.0f;
  s += textureLod(tex, uv, 0.0f).rgb * 4.0f;
  s += textureLod(tex, uv + d.xw, 0.0f).rgb * 2.0f;
  s += textureLod(tex, uv + d.zy, 0.0f).rgb;
  s += textureLod(tex, uv + d.wy, 0.0f).rgb * 2.0f;
  s += textureLod(tex, uv + d.xy, 0.0f).rgb;
  return s * (1.0f / 16.0f);
}
