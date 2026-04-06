/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Bloom Composite Compute Shader
 *
 * Additively blends the bloom pyramid base level (bloom_tx) onto the HDR
 * combined scene buffer (combined_img) in a single pass.
 *
 * For each output pixel:
 *   combined_img[p] = combined_img[p] + texture(bloom_tx, uv) * bloom_intensity
 *
 * bloom_tx is at render_res / 2 (the base of the pyramid after upsample).
 * We sample it with bilinear filtering to produce the full-resolution contribution.
 * This is equivalent to what UE4 and Frostbite do in their "Bloom Composite" pass.
 *
 * Using imageLoad/imageStore on combined_img (READ_WRITE) avoids allocating
 * a separate temporary texture and saves one full-resolution RGBA16F roundtrip
 * (~4 MB at 1080p, ~8 MB at 1440p) compared to a ping-pong approach.
 *
 * Local group size 8×8 = 64 threads: fits in one NVIDIA warp-pair and one AMD
 * wavefront, maximising occupancy with minimal shared-memory pressure.
 */

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

/* bloom_tx: the base of the bloom pyramid (render_res / 2, RGBA16F).
 * Sampled with bilinear interpolation to produce full-resolution contribution. */
uniform sampler2D bloom_tx;

/* combined_img: the HDR scene color buffer (render_res, RGBA16F).
 * READ_WRITE: we read the existing value and add bloom to it in-place. */
layout(rgba16f) uniform image2D combined_img;

/* Bloom blend weight in [0, 1].  Applied uniformly to all pixels that survive
 * the threshold in the downsample pass.  Typical AAA values: 0.03 – 0.08. */
uniform float bloom_intensity;

void main()
{
  const ivec2 out_px  = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 out_res = imageSize(combined_img);

  /* Guard against threads launched beyond the image boundary (happens when
   * resolution is not a multiple of the group size). */
  if (any(greaterThanEqual(out_px, out_res))) {
    return;
  }

  /* Convert pixel coordinates to [0, 1] UVs for the bilinear bloom sample.
   * Offset by 0.5 to sample at the pixel centre. */
  const vec2 uv = (vec2(out_px) + 0.5) / vec2(out_res);

  /* Bilinear sample: bloom_tx is at half resolution, so a single tap here
   * already averages a 2×2 block of bloom texels — effectively a free tent filter. */
  const vec4 bloom_sample = texture(bloom_tx, uv);

  /* Read the existing HDR radiance for this pixel. */
  const vec4 combined = imageLoad(combined_img, out_px);

  /* Additive blend: RGB only.  Alpha is kept from the scene buffer. */
  const vec4 result = vec4(combined.rgb + bloom_sample.rgb * bloom_intensity,
                           combined.a);

  imageStore(combined_img, out_px, result);
}
