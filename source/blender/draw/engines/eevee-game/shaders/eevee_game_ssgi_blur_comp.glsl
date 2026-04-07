/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * SSGI Bilateral Blur and Upsample
 *
 * Upsamples the half-resolution SSGI radiance to full resolution and applies
 * a separable bilateral blur to suppress the structured noise from the Halton
 * sampling in the main trace pass.
 *
 * Design: joint bilateral upsample using depth similarity as the cross-guide.
 * This is the same filter as the GTAO upsample but operating on RGBA16F
 * radiance rather than R8 AO.
 *
 * The blur kernel is a 3×3 Gaussian (σ=1) weighted by depth similarity.
 * A separable 2-pass implementation (H then V) would be 50% cheaper but
 * requires an intermediate RGBA16F buffer.  The non-separable 3×3 version
 * costs ~9 texture fetches and is compute-bound rather than bandwidth-bound
 * at half resolution, so the single-pass version is chosen for simplicity.
 *
 * Local group 8×8.
 */

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

uniform sampler2D ssgi_lowres_tx; /* RGBA16F half-res SSGI radiance */
uniform sampler2D depth_tx;       /* Full-res R32F NDC depth (bilateral guide) */

layout(rgba16f) uniform writeonly image2D out_ssgi_final_img; /* Full-res output */

uniform vec2  z_planes;
uniform vec2  screen_res;

/* Depth threshold for the bilateral filter.
 * Neighbours with depth difference > DEPTH_SIGMA are suppressed.
 * 0.15 m prevents GI from bleeding across walls and floors. */
#define DEPTH_SIGMA 0.15

/* Gaussian kernel weights for a 3×3 (σ=1) filter.
 * Pre-normalised: 9 weights, sum = 1.0. */
const float GAUSS_3x3[9] = float[9](
  0.0625, 0.125, 0.0625,
  0.125,  0.25,  0.125,
  0.0625, 0.125, 0.0625
);

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
  const ivec2 out_res = imageSize(out_ssgi_final_img);

  if (any(greaterThanEqual(out_px, out_res))) {
    return;
  }

  const float z_near = z_planes.x;
  const float z_far  = z_planes.y;

  const vec2 full_uv   = (vec2(out_px) + 0.5) / screen_res;
  const vec2 half_size = vec2(textureSize(ssgi_lowres_tx, 0));
  const vec2 half_texel = 1.0 / half_size;

  /* Reference depth for this output pixel. */
  float ref_ndc = texture(depth_tx, full_uv).r;
  float ref_lin = linear_depth(ref_ndc, z_near, z_far);

  /* Sky: no indirect light. */
  if (ref_ndc >= 0.9999) {
    imageStore(out_ssgi_final_img, out_px, vec4(0.0));
    return;
  }

  /* 3×3 bilateral Gaussian over the half-res SSGI radiance.
   * Each tap is weighted by its Gaussian spatial weight × depth similarity. */
  vec3  radiance_sum = vec3(0.0);
  float weight_sum   = 0.0;
  int   tap          = 0;

  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      vec2 sample_uv = full_uv + vec2(float(dx), float(dy)) * half_texel;

      /* Clamp to avoid out-of-bounds texture reads at screen edges. */
      sample_uv = clamp(sample_uv, half_texel * 0.5, vec2(1.0) - half_texel * 0.5);

      vec3  radiance   = texture(ssgi_lowres_tx, sample_uv).rgb;

      /* Bilateral depth weight: use full-res depth at the corresponding sample position.
       * This guides the filter to respect depth edges at full resolution. */
      float sample_ndc = texture(depth_tx, sample_uv).r;
      float sample_lin = linear_depth(sample_ndc, z_near, z_far);

      float depth_diff = ref_lin - sample_lin;
      float depth_w    = exp(-(depth_diff * depth_diff) /
                              (DEPTH_SIGMA * DEPTH_SIGMA));

      float w = GAUSS_3x3[tap] * depth_w;

      radiance_sum += radiance * w;
      weight_sum   += w;

      tap++;
    }
  }

  vec3 result = (weight_sum > 1e-5) ?
      radiance_sum / weight_sum :
      texture(ssgi_lowres_tx, full_uv).rgb;  /* Fallback: no bilateral */

  imageStore(out_ssgi_final_img, out_px, vec4(result, 1.0));
}
