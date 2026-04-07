/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * GTAO Bilateral Upsample Compute Shader
 *
 * Upsamples the half-resolution AO texture to full resolution, using depth
 * similarity as the bilateral weight to prevent AO from "bleeding" across
 * geometry edges (the primary artifact of naive bilinear upsampling).
 *
 * Algorithm: Joint bilateral upsample (cross-bilateral filter).
 *
 * For each full-res output pixel:
 *   1. Find the 4 surrounding half-res AO texels (2×2 neighbourhood).
 *   2. Fetch their depths from the full-res depth buffer at matching positions.
 *   3. Weight each AO sample by exp(-|depth_full - depth_half|²  / σ_depth²).
 *   4. Normalise and write.
 *
 * The depth similarity weight forces the filter to prefer AO samples from
 * geometry at the same depth as the current pixel, so edges in the depth map
 * produce sharp edges in the upsampled AO — no bleeding.
 *
 * σ_depth (DEPTH_SIGMA): controls how aggressively edges suppress cross-boundary
 * samples.  0.1 m ≈ good for interior scenes; 0.5 m for large outdoor environments.
 *
 * Local group 8×8.
 */

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

uniform sampler2D ao_lowres_tx; /* R8, half-resolution GTAO result */
uniform sampler2D depth_tx;     /* R32F full-resolution NDC depth */

layout(r8) uniform writeonly image2D out_ao_final_img; /* R8, full resolution */

uniform vec2 z_planes;    /* x = z_near, y = z_far */
uniform vec2 screen_res;  /* Full-resolution dimensions */

/* Bilateral depth similarity threshold.
 * Neighbour contributions are suppressed when their linear depth differs
 * by more than DEPTH_SIGMA from the target pixel's depth. */
#define DEPTH_SIGMA 0.2

/* ------------------------------------------------------------------ */

float linear_depth(float ndc_d, float z_near, float z_far)
{
  return (z_near * z_far) / (z_far - ndc_d * (z_far - z_near));
}

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

void main()
{
  const ivec2 out_px  = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 out_res = imageSize(out_ao_final_img);

  if (any(greaterThanEqual(out_px, out_res))) {
    return;
  }

  const float z_near = z_planes.x;
  const float z_far  = z_planes.y;

  /* Full-res UV for this pixel. */
  const vec2 full_uv = (vec2(out_px) + 0.5) / screen_res;

  /* Current pixel's linear depth (reference for bilateral weighting). */
  float ref_ndc   = texture(depth_tx, full_uv).r;
  float ref_lin   = linear_depth(ref_ndc, z_near, z_far);

  /* Sky: no AO needed. */
  if (ref_ndc >= 0.9999) {
    imageStore(out_ao_final_img, out_px, vec4(1.0));
    return;
  }

  /* Half-res texel size in UV space. */
  const vec2 half_res  = vec2(textureSize(ao_lowres_tx, 0));
  const vec2 half_texel = 1.0 / half_res;

  /* Map full-res UV to the corresponding position in the half-res grid.
   * The four surrounding half-res texels share the same 2×2 full-res block. */
  vec2 half_uv     = full_uv;  /* Same UV, AO tex is at half the resolution. */
  vec2 half_offset = half_texel * 0.5;

  /* 2×2 neighbourhood offsets in half-res UV space. */
  vec2 offsets[4];
  offsets[0] = vec2(-half_offset.x, -half_offset.y);
  offsets[1] = vec2( half_offset.x, -half_offset.y);
  offsets[2] = vec2(-half_offset.x,  half_offset.y);
  offsets[3] = vec2( half_offset.x,  half_offset.y);

  float ao_sum     = 0.0;
  float weight_sum = 0.0;

  for (int i = 0; i < 4; i++) {
    vec2  sample_uv  = half_uv + offsets[i];
    float sample_ao  = texture(ao_lowres_tx, sample_uv).r;

    /* Depth of this half-res sample (sample at the corresponding full-res position). */
    vec2  full_sample_uv  = half_uv + offsets[i];  /* Same UV domain — half tex bilinear */
    float sample_ndc      = texture(depth_tx, full_sample_uv).r;
    float sample_lin      = linear_depth(sample_ndc, z_near, z_far);

    /* Bilateral weight: Gaussian in depth space.
     * Samples at very different depths from the output pixel are suppressed.
     * This prevents AO computed on a wall from bleeding into the floor. */
    float depth_diff = ref_lin - sample_lin;
    float weight = exp(-(depth_diff * depth_diff) / (DEPTH_SIGMA * DEPTH_SIGMA));

    ao_sum     += sample_ao * weight;
    weight_sum += weight;
  }

  /* Fallback: if all neighbours are suppressed (extreme depth discontinuity),
   * use the centre sample unweighted.  Avoids division by zero. */
  float ao;
  if (weight_sum < 1e-4) {
    ao = texture(ao_lowres_tx, half_uv).r;
  }
  else {
    ao = ao_sum / weight_sum;
  }

  imageStore(out_ao_final_img, out_px, vec4(ao, 0.0, 0.0, 0.0));
}
