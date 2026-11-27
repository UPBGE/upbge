/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Depth of Field — Fast Approximation Compute Shader
 *
 * Three-pass algorithm controlled by the DOF_STAGE preprocessor define:
 *
 *   DOF_STAGE == 0  (COC_SETUP)
 *     One thread per full-res pixel.
 *     Reads hardware depth, reconstructs view-space Z, derives the Circle of
 *     Confusion radius in screen-space pixels.
 *     Positive CoC  → background (behind focus plane, blur).
 *     Negative CoC  → foreground (in front of focus plane, blur).
 *     |CoC| ≈ 0     → in-focus.
 *     Output: R16F CoC texture at full resolution.
 *
 *   DOF_STAGE == 1  (BOKEH_BLUR)
 *     One thread per half-res pixel.
 *     Poisson-disk blur on the downsampled scene color, weighted by CoC.
 *     16-tap Poisson disk: good perceptual quality, avoids structured patterns.
 *     Output: RGBA16F half-res blurred color.
 *
 *   DOF_STAGE == 2  (DOF_RESOLVE)
 *     One thread per full-res pixel.
 *     Bilateral blend between sharp and blurred images.
 *     Alpha weight is computed from the per-pixel CoC so in-focus pixels are
 *     taken 100% from the sharp buffer and large-CoC pixels from the blurred one.
 *     Foreground (negative CoC) gets extra spread weight to avoid "halo" leaking.
 *     Output: RGBA16F full-res combined image.
 *
 * This three-pass design is the same as Guerrilla Games / Killzone's real-time DoF
 * (Jimenez 2010).  The half-res blur is the key performance win: reduces bokeh
 * sample cost by 4× with minimal quality loss at typical bokeh radii.
 *
 * Local group 8×8 = 64 threads: one warp-pair / wavefront.
 */

#include "infos/eevee_game_infos.hh"

/* Adjust according to DOF_STAGE for C++ compilation. */
COMPUTE_SHADER_CREATE_INFO(eevee_game_dof_coc_setup)

#include "eevee_reverse_z_lib.glsl"

/* ------------------------------------------------------------------ */
/* Focus parameters
 *
 *   focus_params.x = 1 / focus_distance (scene units)
 *   focus_params.y = coc_scale: aperture radius times pixels per scene unit at unit distance
 *   focus_params.z = max_bokeh_radius (pixels at render resolution)
 *   focus_params.w = unused
 *
 * Thin lens CoC radius in pixels, as EEVEE:
 *
 *   CoC_px = coc_scale * (1 / focus_distance - 1 / depth)
 *
 * clamped to max_bokeh_radius. */

/* z_planes = (z_near, z_far), for linear depth reconstruction:
 *   linear_z = (z_near * z_far) / (z_far - depth * (z_far - z_near)) */

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Reconstruct linear view-space depth from the non-linear NDC depth buffer.
 * Standard perspective-correct formula: maps [0,1] NDC Z to [near, far]. */
float linear_depth(float ndc_depth, float z_near, float z_far)
{
  return (z_near * z_far) / (z_far - ndc_depth * (z_far - z_near));
}

/* ------------------------------------------------------------------ */
/* Poisson disk samples (16 taps, pre-rotated for good low-discrepancy coverage)
 *
 * Golden-angle Fibonacci disk: no structured banding, uniform radial density.
 * Pre-baked rather than computed per-thread: saves ~16 sin/cos calls. */
const float2 POISSON_16[16] = float2[16](
  float2( 0.000000,  0.000000),
  float2( 0.382683,  0.923880),
  float2(-0.707107,  0.707107),
  float2(-0.923880, -0.382683),
  float2( 0.000000, -1.000000),
  float2( 0.923880, -0.382683),
  float2( 0.707107,  0.707107),
  float2(-0.382683,  0.923880),
  float2(-0.500000,  0.000000),
  float2( 0.250000,  0.433013),
  float2( 0.000000, -0.500000),
  float2(-0.250000, -0.433013),
  float2( 0.750000,  0.000000),
  float2(-0.125000,  0.649519),
  float2(-0.625000, -0.216506),
  float2( 0.433013, -0.500000)
);

/* ================================================================== */
/* DOF_STAGE 0 — Circle of Confusion Setup                            */
/* ================================================================== */
#if DOF_STAGE == 0

void main()
{
  const int2 px  = int2(gl_GlobalInvocationID.xy);
  const int2 res = imageSize(out_coc_img);

  if (any(greaterThanEqual(px, res))) {
    return;
  }

  const float2 uv = (float2(px) + 0.5) / float2(res);

  float ndc_depth = reverse_z::read(texture(depth_tx, uv).r);

  float inv_focus   = focus_params.x;
  float coc_scale   = focus_params.y;
  float max_bokeh_r = focus_params.z;

  /* Sky pixels (far plane) are at infinity: 1 / depth = 0. */
  float inv_z = 0.0;
  if (ndc_depth < 0.9999) {
    inv_z = 1.0 / linear_depth(ndc_depth, z_planes.x, z_planes.y);
  }

  /* Signed CoC:
   *   positive → background (depth > focus distance)
   *   negative → foreground (depth < focus distance) */
  float coc = clamp(coc_scale * (inv_focus - inv_z), -max_bokeh_r, max_bokeh_r);

  imageStore(out_coc_img, px, float4(coc, 0.0, 0.0, 0.0));
}

/* ================================================================== */
/* DOF_STAGE 1 — Bokeh Blur (half-resolution)                        */
/* ================================================================== */
#elif DOF_STAGE == 1

void main()
{
  const int2 px  = int2(gl_GlobalInvocationID.xy);
  const int2 res = imageSize(out_bokeh_img);

  if (any(greaterThanEqual(px, res))) {
    return;
  }

  /* Half-res UV: multiply by 2 to get full-res UV for CoC lookup. */
  const float2 uv         = (float2(px) + 0.5) / float2(res);
  const float2 full_uv    = uv;   /* color and coc are both full-res, bilinear OK */
  const float2 texel      = 1.0 / float2(textureSize(in_color_tx, 0));

  float centre_coc = texture(coc_tx, full_uv).r;

  /* Bokeh radius in UV space.  Foreground (negative CoC) and background blur
   * both use |CoC| for the sample spread. */
  float bokeh_r_px = abs(centre_coc);
  float2  bokeh_r_uv = float2(bokeh_r_px) * texel;

  /* Minimum 0.5 px spread so the shader always produces a slightly softened
   * image even at focus distance (avoids aliasing on very sharp edges near focus). */
  bokeh_r_uv = max(bokeh_r_uv, texel * 0.5);

  float4  color_sum   = float4(0.0);
  float weight_sum  = 0.0;

  for (int i = 0; i < 16; i++) {
    float2 sample_uv  = full_uv + POISSON_16[i] * bokeh_r_uv;
    float4 sample_col = texture(in_color_tx, sample_uv);
    float sample_coc = abs(texture(coc_tx, sample_uv).r);

    /* Weight by CoC magnitude: samples with large blur contribute more to the
     * bokeh accumulation.  This biases the blur toward the most out-of-focus
     * pixels and avoids "in-focus edge" leaking into the blurred region. */
    float w = sample_coc + 0.5;

    color_sum  += sample_col * w;
    weight_sum += w;
  }

  imageStore(out_bokeh_img, px, color_sum / max(weight_sum, 1e-4));
}

/* ================================================================== */
/* DOF_STAGE 2 — Resolve (full-resolution bilateral composite)       */
/* ================================================================== */
#elif DOF_STAGE == 2

void main()
{
  const int2 px  = int2(gl_GlobalInvocationID.xy);
  const int2 res = imageSize(out_final_img);

  if (any(greaterThanEqual(px, res))) {
    return;
  }

  const float2 uv = (float2(px) + 0.5) / float2(res);

  float coc        = texture(coc_tx, uv).r;
  float max_r      = focus_params.z;

  /* Blend weight: 0 = fully sharp, 1 = fully bokeh-blurred.
   * The smoothstep provides a soft transition through the depth of field zone. */
  float blur_weight = smoothstep(0.5, min(2.0, max_r), abs(coc));

  /* Foreground pixels (negative CoC) receive a stronger blur weight because
   * they must occlude in-focus geometry behind them.  Without the boost,
   * foreground objects look "too sharp" relative to their CoC value. */
  if (coc < 0.0) {
    blur_weight = clamp(blur_weight * 1.5, 0.0, 1.0);
  }

  float4 sharp  = texture(in_sharp_tx,  uv);
  float4 blurred = texture(in_bokeh_tx,  uv);

  /* Bilateral: the bokeh_tx is at half resolution, so the bilinear fetch here
   * gives a "free" 2× spatial filter over the blurred result — intentional. */
  float4 result = mix(sharp, blurred, blur_weight);

  imageStore(out_final_img, px, result);
}

#endif /* DOF_STAGE */
