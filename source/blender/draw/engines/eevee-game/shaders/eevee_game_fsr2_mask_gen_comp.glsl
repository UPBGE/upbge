/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * FSR3 Reactive Mask Generation
 *
 * Generates the per-pixel "reactive" and "transparency" masks required by
 * AMD FSR 3 / FSR 2 for correct temporal reconstruction.
 *
 * Background:
 *   FSR temporal upscaling accumulates multiple low-resolution frames to
 *   reconstruct a high-resolution output.  Temporally unstable pixels
 *   (particles, alpha-blended surfaces, reflections, translucent glass)
 *   should not be accumulated across frames because their content changes
 *   faster than the reprojection model assumes.
 *
 *   The reactive mask tells FSR to weight the current frame more heavily
 *   for these pixels, preventing ghosting.  The transparency mask tells FSR
 *   which pixels to treat as partially transparent during accumulation.
 *
 * Output textures (R8 unorm, render resolution):
 *   reactive_mask_img   — [0,1]: 0 = fully accumulated, 1 = fully reactive.
 *   transp_mask_img     — [0,1]: 0 = opaque, 1 = fully transparent.
 *
 * Classification heuristics (from AMD FSR2 Best Practices guide):
 *
 *   REACTIVE:
 *     a) Motion vector magnitude: pixels with high screen-space velocity
 *        relative to geometry velocity are likely particles or reflections.
 *     b) Stencil bits: STENCIL_REFRACTIVE and STENCIL_TRANSPARENT flag pixels
 *        that the material system has already identified as reactive.
 *     c) Luma variance: pixels whose luminance changed significantly between
 *        the previous and current frame (indicates temporal instability).
 *
 *   TRANSPARENCY:
 *     a) Stencil STENCIL_TRANSPARENT bit.
 *     b) Alpha value < 1.0 in the combined buffer.
 *
 * In practice, most game engines also have artist-controlled reactive mask
 * painting; the heuristic approach here is a good automatic fallback.
 *
 * Local group 8×8.
 */

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

/* Current frame combined color (HDR, RGBA16F). */
uniform sampler2D color_tx;

/* Motion vector texture (RG16F, screen-space velocity in UV units per frame). */
uniform sampler2D vector_tx;

/* Previous frame color for luma-change detection (RGBA16F). */
uniform sampler2D color_history_tx;

/* Stencil buffer texture (sampled as R8UI or R8 float).
 * Stencil bits match the eevee_game_defines.hh STENCIL_* enum. */
uniform sampler2D stencil_tx;  /* Packed stencil, sampled as float R8 */

/* Output masks at render resolution. */
layout(r8) uniform writeonly image2D reactive_mask_img;
layout(r8) uniform writeonly image2D transp_mask_img;

/* Tuneable thresholds (can be exposed to game settings). */
uniform float reactive_motion_threshold = 0.01;   /* UV/frame velocity above which pixel is reactive */
uniform float reactive_luma_threshold   = 0.2;    /* Luma change above which pixel is reactive */
uniform float reactive_base             = 0.0;    /* Minimum reactive value for all pixels */

/* Stencil bit masks (match C++ StencilBits enum). */
#define STENCIL_TRANSPARENT    (1 << 1)
#define STENCIL_REFRACTIVE     (1 << 3)

/* ------------------------------------------------------------------ */
/* Luminance from linear RGB. */
float luma(vec3 c)
{
  return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

void main()
{
  const ivec2 px  = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 res = imageSize(reactive_mask_img);

  if (any(greaterThanEqual(px, res))) {
    return;
  }

  const vec2 uv = (vec2(px) + 0.5) / vec2(res);

  /* ---- Sample inputs ---- */
  vec4 color_cur  = texture(color_tx, uv);
  vec4 color_prev = texture(color_history_tx, uv);
  vec2 motion     = texture(vector_tx, uv).xy;  /* Screen-space UV velocity */
  float stencil_f = texture(stencil_tx, uv).r;

  /* Decode stencil: stored as float [0, 255/255] → uint8 value. */
  uint stencil_bits = uint(stencil_f * 255.0 + 0.5);

  /* ---- Reactive mask computation ---- */

  /* a) Motion-based reactivity.
   *    High screen-space velocity relative to scene geometry typically means
   *    particles, decals, or alpha-blended emitters that don't match the
   *    static geometry reprojection.  We measure raw motion magnitude. */
  float motion_magnitude = length(motion);
  float reactive_motion  = smoothstep(reactive_motion_threshold * 0.5,
                                      reactive_motion_threshold * 2.0,
                                      motion_magnitude);

  /* b) Stencil-based reactivity.
   *    Refractive and transparent pixels are unconditionally reactive. */
  float reactive_stencil = 0.0;
  if ((stencil_bits & uint(STENCIL_REFRACTIVE)) != 0u ||
      (stencil_bits & uint(STENCIL_TRANSPARENT)) != 0u)
  {
    reactive_stencil = 1.0;
  }

  /* c) Luma-change based reactivity.
   *    If the pixel's brightness changed significantly from last frame,
   *    it is likely a light source, particle, or animated material.
   *    We compare against the history buffer (no reprojection — good enough
   *    for stationary or slow-moving reactive elements). */
  float luma_cur  = luma(color_cur.rgb);
  float luma_prev = luma(color_prev.rgb);
  float luma_delta = abs(luma_cur - luma_prev) / max(luma_cur + luma_prev, 0.1);
  float reactive_luma = smoothstep(reactive_luma_threshold * 0.5,
                                   reactive_luma_threshold * 2.0,
                                   luma_delta);

  /* Combine reactive sources: max gives the strongest signal.
   * We do NOT sum them because that would over-react and wash out
   * temporal accumulation on surfaces that are only mildly reactive. */
  float reactive = max(reactive_base,
                   max(reactive_motion,
                   max(reactive_stencil,
                       reactive_luma)));

  reactive = clamp(reactive, 0.0, 1.0);

  /* ---- Transparency mask computation ---- */

  /* a) Stencil transparent bit. */
  float transp_stencil = ((stencil_bits & uint(STENCIL_TRANSPARENT)) != 0u) ? 1.0 : 0.0;

  /* b) Alpha channel: values < 1.0 indicate blended geometry. */
  float transp_alpha = 1.0 - color_cur.a;  /* 0 = opaque, 1 = fully transparent */

  float transp = clamp(max(transp_stencil, transp_alpha), 0.0, 1.0);

  /* ---- Write outputs ---- */
  imageStore(reactive_mask_img, px, vec4(reactive, 0.0, 0.0, 0.0));
  imageStore(transp_mask_img,   px, vec4(transp,   0.0, 0.0, 0.0));
}
