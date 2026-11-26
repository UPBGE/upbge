/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Helpers shared by the temporal store and resolve passes.
 */

#pragma once

float max3(float3 v)
{
  return max(v.x, max(v.y, v.z));
}

float3 rgb_to_ycocg(float3 rgb)
{
  return float3(dot(rgb, float3(0.25, 0.5, 0.25)),
                dot(rgb, float3(0.5, 0.0, -0.5)),
                dot(rgb, float3(-0.25, 0.5, -0.25)));
}

float3 ycocg_to_rgb(float3 ycocg)
{
  return float3(ycocg.x + ycocg.y - ycocg.z, ycocg.x + ycocg.z, ycocg.x - ycocg.y - ycocg.z);
}

/* Reversible tone map of strength s (0 = linear, 1 = c / (1 + max(c))), then YCoCg. */
float3 color_to_space(float3 rgb, float s)
{
  rgb = max(rgb, float3(0.0));
  return rgb_to_ycocg(rgb * (1.0 / (1.0 + s * max3(rgb))));
}

float3 space_to_color(float3 ycocg, float s)
{
  float3 rgb = max(ycocg_to_rgb(ycocg), float3(0.0));
  /* s * max3(rgb) < 1 by construction; the floor keeps the result inside half float range. */
  return rgb * (1.0 / max(1.0 - s * max3(rgb), 1.0 / 65504.0));
}

/* Inverse of the view depth (1 / positive distance) from NDC depth. params: winmat[2][2],
 * winmat[3][2], is perspective. Perspective: depth = B / (ndc + A), so its inverse is affine in
 * NDC and the window reprojects with no division. Orthographic: depth = (B - ndc) / A.
 * Comparisons of depth ratios hold for inverses, with min and max swapped. 0 is infinity. */
float inverse_depth(float ndc_z, float4 params)
{
  return (params.z != 0.0) ? (ndc_z + params.x) / params.y : params.x / (params.y - ndc_z);
}

/* NDC depth from inverse_depth(). Perspective with 0 (background) gives the NDC depth of
 * infinity, -A: reprojected with it, a point follows the camera rotation only. */
float ndc_from_inverse_depth(float inverse, float4 params)
{
  return (params.z != 0.0) ? params.y * inverse - params.x : params.y - params.x / inverse;
}

/* From a reversed depth buffer value (1 = near, 0 = far). */
float inverse_depth_buffer(float depth_buffer, float4 params)
{
  return inverse_depth((1.0 - depth_buffer) * 2.0 - 1.0, params);
}

/* NaN and Inf removed (one would live in the history forever), clamped to the range of the
 * ring and history formats. */
float3 sanitize_color(float3 color)
{
  if (any(isnan(color)) || any(isinf(color))) {
    return float3(0.0);
  }
  return clamp(color, float3(0.0), float3(65000.0));
}

/* The median luma (tone mapped, in [0, 1)) is stored in 8 bits as its square root: steps are
 * finer in the darks, where the ratio test against MEDIAN_KILL_RATIO is most sensitive. */
float median_encode(float luma)
{
  return sqrt(clamp(luma, 0.0, 1.0));
}

float median_decode(float stored)
{
  return stored * stored;
}

/* Ring color, RGBA8: rgb = square root of c / (1 + max(c)), a = median_encode() of the median.
 * Against R11G11B10F color plus R8 median: same error on the CPU reference scenes. 6 bit
 * channels (RGB565) lost 0.2 to 0.3 dB, 4:2:0 chroma 1.3 to 3.3 dB.
 * Colors brighter than RING_COLOR_MAX are scaled down to it, hue kept: past 254 the top code
 * would decode as infinity. Only the window's past frames see the limit. */
#define RING_COLOR_MAX 126.0
float4 ring_color_encode(float3 linear, float median)
{
  /* max(c) <= RING_COLOR_MAX: 1 + max(c). Above: max(c) * (1 + 1 / RING_COLOR_MAX), so the
   * largest channel stores RING_COLOR_MAX / (1 + RING_COLOR_MAX), code 254. */
  const float m = max3(linear);
  const float norm = max(1.0 + m, m * (1.0 + 1.0 / RING_COLOR_MAX));
  return float4(sqrt(linear * (1.0 / norm)), median_encode(median));
}

/* Inverse of ring_color_encode() from the packed bytes b: t = (b / 255)^2, c = t / (1 - max(t))
 * = b^2 / (255^2 - max(b^2)). Returns b^2 and the inverse of the denominator, so a weighted
 * sum needs one product for the weight: c * w = b^2 * (w * inverse). Codes stay below 255:
 * the denominator is at least 255^2 - 254^2. */
float3 ring_color_squares(uint color)
{
  const float3 b = float3(color & 0xFFu, (color >> 8) & 0xFFu, (color >> 16) & 0xFFu);
  return b * b;
}

float ring_color_inverse_scale(float3 squares)
{
  return 1.0 / (65025.0 - max3(squares));
}

float ring_median_decode(float4 stored)
{
  return median_decode(stored.a);
}

/* Ring texel, RG32UI: x = info word, y = packUnorm4x8(ring_color_encode()). One fetch per sample
 * gives everything the window needs. */
uint4 ring_texel_pack(uint info, float3 linear, float median)
{
  return uint4(info, packUnorm4x8(ring_color_encode(linear, median)), 0u, 0u);
}

/* Ring info word of a surface (the background is 0), see TEMPORAL_INFO_UNSTABLE_BIT. */
uint info_pack(uint key, float inverse, bool unstable)
{
  /* Key 0 is the background in the ring: unknown resources take TEMPORAL_KEY_MASK, which no
   * synced object uses. */
  key &= TEMPORAL_KEY_MASK;
  key = (key != 0u) ? key : TEMPORAL_KEY_MASK;
  /* Half float bits of the inverse depth go to the high half; the sign is the unstable flag,
   * the lowest mantissa bit (0.1 %) the transparent flag. Clamped above 0, the background. */
  uint depth_bits = packHalf2x16(float2(clamp(inverse, 1e-7, 65000.0), 0.0)) & 0x7FFEu;
  return key | (depth_bits << 16) | (unstable ? TEMPORAL_INFO_UNSTABLE_BIT : 0u);
}

bool info_is_background(uint info)
{
  return (info & TEMPORAL_KEY_MASK) == 0u;
}

bool info_is_unstable(uint info)
{
  return (info & TEMPORAL_INFO_UNSTABLE_BIT) != 0u;
}

uint info_key(uint info)
{
  return info & TEMPORAL_KEY_MASK;
}

/* Inverse depth, 0 for the background. The absolute value drops the unstable flag, which is the
 * half float sign: no mask. */
float info_inverse_depth(uint info)
{
  return abs(unpackHalf2x16(info >> 16).x);
}

void sort2(float &a, float &b)
{
  float t = min(a, b);
  b = max(a, b);
  a = t;
}

/* Median of 9 in 19 compare-exchanges (Paeth / Devillard network). */
float median9(float v[9])
{
  sort2(v[1], v[2]);
  sort2(v[4], v[5]);
  sort2(v[7], v[8]);
  sort2(v[0], v[1]);
  sort2(v[3], v[4]);
  sort2(v[6], v[7]);
  sort2(v[1], v[2]);
  sort2(v[4], v[5]);
  sort2(v[7], v[8]);
  sort2(v[0], v[3]);
  sort2(v[5], v[8]);
  sort2(v[4], v[7]);
  sort2(v[3], v[6]);
  sort2(v[1], v[4]);
  sort2(v[2], v[5]);
  sort2(v[4], v[7]);
  sort2(v[4], v[2]);
  sort2(v[6], v[4]);
  sort2(v[4], v[2]);
  return v[4];
}

/* Whether two tone mapped values are within `ratio` of each other, safe at zero. No division:
 * max(a, b) / min(a, b) <= ratio. */
bool luma_within(float a, float b, float ratio)
{
  return max(a, b) <= ratio * max(min(a, b), 1e-4);
}
