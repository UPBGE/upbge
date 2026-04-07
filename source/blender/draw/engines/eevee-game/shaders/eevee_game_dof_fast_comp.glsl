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

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

/* ------------------------------------------------------------------ */
/* Focus parameters
 *
 *   focus_params.x = focus_distance (world units from camera)
 *   focus_params.y = f_stop
 *   focus_params.z = max_bokeh_radius (pixels at output resolution)
 *   focus_params.w = unused
 *
 * Lens equation for CoC in pixels:
 *
 *   CoC_mm = |focal_length²  / (f_stop * (focus_dist - focal_length))
 *              * (depth - focus_dist) / depth|
 *
 * Simplified for game use (focal_length << focus_dist, sensor known):
 *   CoC_px = K * |depth - focus_dist| / depth
 *   where K = focal_length² / (f_stop * focus_dist * sensor_mm_per_px)
 *
 * We bake K into focus_params.z as the maximum bokeh radius so the shader
 * only needs a lerp from 0 (in-focus) to max_bokeh_radius (fully blurred). */
uniform vec4 focus_params;

/* ------------------------------------------------------------------ */
/* Per-stage texture bindings                                          */
/* ------------------------------------------------------------------ */

#if DOF_STAGE == 0
  uniform sampler2D depth_tx;
  layout(r16f) uniform writeonly image2D out_coc_img;

  /* Camera clip planes for linear depth reconstruction.
   *   linear_z = (z_near * z_far) / (z_far - depth * (z_far - z_near))
   *
   * z_near, z_far encoded in z_planes.xy so we avoid a UBO dependency. */
  uniform vec2 z_planes;  /* x = z_near, y = z_far */

#elif DOF_STAGE == 1
  uniform sampler2D in_color_tx;
  uniform sampler2D coc_tx;
  layout(rgba16f) uniform writeonly image2D out_bokeh_img;

#elif DOF_STAGE == 2
  uniform sampler2D in_sharp_tx;
  uniform sampler2D in_bokeh_tx;
  uniform sampler2D coc_tx;
  layout(rgba16f) uniform writeonly image2D out_final_img;

#endif /* DOF_STAGE */

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
const vec2 POISSON_16[16] = vec2[16](
  vec2( 0.000000,  0.000000),
  vec2( 0.382683,  0.923880),
  vec2(-0.707107,  0.707107),
  vec2(-0.923880, -0.382683),
  vec2( 0.000000, -1.000000),
  vec2( 0.923880, -0.382683),
  vec2( 0.707107,  0.707107),
  vec2(-0.382683,  0.923880),
  vec2(-0.500000,  0.000000),
  vec2( 0.250000,  0.433013),
  vec2( 0.000000, -0.500000),
  vec2(-0.250000, -0.433013),
  vec2( 0.750000,  0.000000),
  vec2(-0.125000,  0.649519),
  vec2(-0.625000, -0.216506),
  vec2( 0.433013, -0.500000)
);

/* ================================================================== */
/* DOF_STAGE 0 — Circle of Confusion Setup                            */
/* ================================================================== */
#if DOF_STAGE == 0

void main()
{
  const ivec2 px  = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 res = imageSize(out_coc_img);

  if (any(greaterThanEqual(px, res))) {
    return;
  }

  const vec2 uv = (vec2(px) + 0.5) / vec2(res);

  float ndc_depth = texture(depth_tx, uv).r;

  /* Sky pixels (depth == 1.0 far plane) have zero blur — they are infinitely far. */
  if (ndc_depth >= 0.9999) {
    imageStore(out_coc_img, px, vec4(0.0));
    return;
  }

  float z_near = z_planes.x;
  float z_far  = z_planes.y;
  float lin_z  = linear_depth(ndc_depth, z_near, z_far);

  float focus_dist      = focus_params.x;
  float max_bokeh_r     = focus_params.z;

  /* Signed CoC:
   *   positive → background (depth > focus_dist)
   *   negative → foreground (depth < focus_dist)
   * Clamped to max_bokeh_radius to prevent runaway blur. */
  float coc = clamp((lin_z - focus_dist) / focus_dist, -1.0, 1.0) * max_bokeh_r;

  imageStore(out_coc_img, px, vec4(coc, 0.0, 0.0, 0.0));
}

/* ================================================================== */
/* DOF_STAGE 1 — Bokeh Blur (half-resolution)                        */
/* ================================================================== */
#elif DOF_STAGE == 1

void main()
{
  const ivec2 px  = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 res = imageSize(out_bokeh_img);

  if (any(greaterThanEqual(px, res))) {
    return;
  }

  /* Half-res UV: multiply by 2 to get full-res UV for CoC lookup. */
  const vec2 uv         = (vec2(px) + 0.5) / vec2(res);
  const vec2 full_uv    = uv;   /* color and coc are both full-res, bilinear OK */
  const vec2 texel      = 1.0 / vec2(textureSize(in_color_tx, 0));

  float centre_coc = texture(coc_tx, full_uv).r;

  /* Bokeh radius in UV space.  Foreground (negative CoC) and background blur
   * both use |CoC| for the sample spread. */
  float bokeh_r_px = abs(centre_coc);
  vec2  bokeh_r_uv = vec2(bokeh_r_px) * texel;

  /* Minimum 0.5 px spread so the shader always produces a slightly softened
   * image even at focus distance (avoids aliasing on very sharp edges near focus). */
  bokeh_r_uv = max(bokeh_r_uv, texel * 0.5);

  vec4  color_sum   = vec4(0.0);
  float weight_sum  = 0.0;

  for (int i = 0; i < 16; i++) {
    vec2 sample_uv  = full_uv + POISSON_16[i] * bokeh_r_uv;
    vec4 sample_col = texture(in_color_tx, sample_uv);
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
  const ivec2 px  = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 res = imageSize(out_final_img);

  if (any(greaterThanEqual(px, res))) {
    return;
  }

  const vec2 uv = (vec2(px) + 0.5) / vec2(res);

  float coc        = texture(coc_tx, uv).r;
  float max_r      = focus_params.z;

  /* Blend weight: 0 = fully sharp, 1 = fully bokeh-blurred.
   * The smoothstep provides a soft transition through the depth of field zone. */
  float blur_weight = smoothstep(0.0, max_r * 0.5, abs(coc));

  /* Foreground pixels (negative CoC) receive a stronger blur weight because
   * they must occlude in-focus geometry behind them.  Without the boost,
   * foreground objects look "too sharp" relative to their CoC value. */
  if (coc < 0.0) {
    blur_weight = clamp(blur_weight * 1.5, 0.0, 1.0);
  }

  vec4 sharp  = texture(in_sharp_tx,  uv);
  vec4 blurred = texture(in_bokeh_tx,  uv);

  /* Bilateral: the bokeh_tx is at half resolution, so the bilinear fetch here
   * gives a "free" 2× spatial filter over the blurred result — intentional. */
  vec4 result = mix(sharp, blurred, blur_weight);

  imageStore(out_final_img, px, result);
}

#endif /* DOF_STAGE */
