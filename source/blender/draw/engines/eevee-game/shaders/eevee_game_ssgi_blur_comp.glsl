/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * SSGI denoise: bilateral 3x3 Gaussian at half resolution.
 *
 * Smooths the structured noise of the few rays per pixel of the trace, weighted by depth
 * similarity so it does not bleed across depth edges. It stays at half resolution: the lighting
 * pass upsamples with a 2x2 depth aware filter as it reads, which saves writing and reading
 * back a full resolution RGBA16F target.
 *
 * The depth of a half resolution texel is mip 1 of the Hi-Z pyramid, which is aligned with the
 * half resolution grid.
 *
 * Local group 8x8.
 */

#include "infos/eevee_game_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_game_ssgi_blur)

/* Depth difference (world units) at which a neighbour's weight drops to 1/e. 0.15 m keeps the
 * result from bleeding across walls and floors. */
#define DEPTH_SIGMA 0.15

float linear_depth(float depth)
{
  return (z_planes.x * z_planes.y) / (z_planes.y - depth * (z_planes.y - z_planes.x));
}

void main()
{
  const int2 out_px = int2(gl_GlobalInvocationID.xy);
  const int2 out_res = imageSize(out_ssgi_img);
  if (any(greaterThanEqual(out_px, out_res))) {
    return;
  }

  /* Mip 1 of the pyramid: the half resolution opaque depth, conventional range. */
  float ref_depth = texelFetch(hiz_tx, out_px, 1).r;
  if (ref_depth >= 1.0f) {
    imageStore(out_ssgi_img, out_px, float4(0.0f));
    return;
  }
  float ref_lin = linear_depth(ref_depth);

  float4 sum = float4(0.0f);
  float weight_sum = 0.0f;
  for (int y = -1; y <= 1; y++) {
    for (int x = -1; x <= 1; x++) {
      int2 texel = clamp(out_px + int2(x, y), int2(0), out_res - 1);
      /* Gaussian sigma 1: 1/4, 1/8, 1/16 from the center out. */
      float spatial = ((x == 0) ? 2.0f : 1.0f) * ((y == 0) ? 2.0f : 1.0f) * 0.0625f;
      float delta = ref_lin - linear_depth(texelFetch(hiz_tx, texel, 1).r);
      float w = spatial * exp(-(delta * delta) * (1.0f / (DEPTH_SIGMA * DEPTH_SIGMA)));
      sum += texelFetch(ssgi_lowres_tx, texel, 0) * w;
      weight_sum += w;
    }
  }
  /* The center weighs 1/4 at least: weight_sum is never zero. */
  imageStore(out_ssgi_img, out_px, sum / weight_sum);
}
