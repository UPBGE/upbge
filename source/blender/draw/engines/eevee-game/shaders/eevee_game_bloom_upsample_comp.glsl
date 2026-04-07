/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Bloom Upsample Compute Shader
 *
 * Iteratively upsamples the bloom pyramid from coarsest to finest level,
 * additively accumulating each coarser level into the one above it.
 *
 * Filter kernel: 3×3 tent (bilinear fetch with ±radius offset).
 * The tent provides smooth spatial falloff and is free to evaluate because
 * we exploit the GPU's bilinear hardware:
 *
 *   Four bilinear taps at (±r, ±r) implicitly cover a 2r×2r support region.
 *   Weighting them equally gives a box; sampling at ±r/2 gives a tent.
 *
 * The additive accumulation means that the coarsest level (broadest glow)
 * is always present in the final composite regardless of pyramid depth.
 * Each subsequent finer level adds narrower, brighter features on top.
 *
 * The output image (out_color_img) is the next-finer pyramid level
 * which is READ_WRITE: we add to the existing content (set by the previous
 * downsample pass) rather than overwriting it.
 *
 * radius: controls the tent filter sample spread; drives "bloom size".
 *   Typical range: 0.4 – 1.5 texels of the output image.
 *
 * Local group 8×8: one NVIDIA warp-pair / one AMD wavefront.
 */

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

/* Source: the coarser pyramid level (half the output resolution). */
uniform sampler2D in_blur_tx;

/* Destination: the next-finer pyramid level.  READ_WRITE because we add
 * the upsampled coarser level to the existing content from the downsample pass. */
layout(rgba16f) uniform image2D out_color_img;

/* Tent filter radius in UV space (fraction of output texture size).
 * Converted to absolute texels per sample offset below. */
uniform float radius;

/* ------------------------------------------------------------------ */
/* 3×3 tent upsample (9-tap, bilinear hardware)
 *
 * The tent filter approximates a Gaussian with a single ring of bilinear
 * samples.  For bloom it is perceptually indistinguishable from a true
 * Gaussian and ~4× cheaper.
 *
 * Weight grid (all equal = 1/9):
 *   1 1 1
 *   1 1 1   → normalise so sum = 1.
 *   1 1 1
 *
 * We implement the "9-tap bilinear trick" from Jorge Jimenez 2014:
 * sample at integer offsets, let the hardware bilinear do sub-texel
 * interpolation.  Each integer tap covers a 2×2 footprint. */
vec3 tent_upsample(sampler2D tex, vec2 uv, vec2 texel)
{
  /* 3×3 ring at ±texel offsets, 9 samples total. */
  vec3 sum = vec3(0.0);

  for (int y = -1; y <= 1; y++) {
    for (int x = -1; x <= 1; x++) {
      sum += texture(tex, uv + vec2(float(x), float(y)) * texel * radius).rgb;
    }
  }

  /* All 9 taps have equal weight in the tent approximation. */
  return sum * (1.0 / 9.0);
}

void main()
{
  const ivec2 out_px  = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 out_res = imageSize(out_color_img);

  if (any(greaterThanEqual(out_px, out_res))) {
    return;
  }

  /* UV in the output image. The source (in_blur_tx) is at half this resolution,
   * so the same UV directly samples the coarser level — no UV scaling needed. */
  const vec2 uv    = (vec2(out_px) + 0.5) / vec2(out_res);
  const vec2 texel = 1.0 / vec2(textureSize(in_blur_tx, 0));

  vec3 upsampled = tent_upsample(in_blur_tx, uv, texel);

  /* Read the existing finer-level content and add the coarser upsampled bloom.
   * This is the "ping-pong-less" additive accumulation that avoids an extra
   * full-resolution RGBA16F allocation per pyramid level. */
  const vec4 existing = imageLoad(out_color_img, out_px);

  imageStore(out_color_img, out_px, vec4(existing.rgb + upsampled, existing.a));
}
