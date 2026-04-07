/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * FXAA 3.11 — Fast Approximate Anti-Aliasing
 *
 * Adapted from Timothy Lottes' FXAA 3.11 (NVIDIA, public domain) for use
 * in the eevee_game pipeline.
 *
 * Key differences from the original Workbench/EEVEE FXAA shaders:
 *   - Input is RGBA16F (HDR linear) not LDR sRGB.
 *   - Luma is computed from scene-linear RGB using Rec.709 coefficients
 *     without the sRGB sqrt approximation (which is wrong in HDR space).
 *   - Alpha channel is preserved (needed for later overlay compositing).
 *   - No subpixel fix needed: FSR/SMAA is preferred for high quality;
 *     FXAA is the cheap option for low-end hardware.
 *
 * Quality setting: FXAA_QUALITY__PRESET 25 (the high-quality preset from
 * the original header).  Reduces blur artifacts compared to preset 10.
 *
 * This is a single-pass fragment shader that runs after tone mapping has
 * been applied (or before, if HDR AA is desired — controlled by the pipeline).
 * In eevee_game it runs on the LDR-tonemapped buffer just before present().
 *
 * Uniforms:
 *   color_tx         — Source color (RGBA, after all post-FX except FXAA).
 *   fxaa_quality_subpix   — Sub-pixel aliasing removal [0, 1].  0.75 default.
 *   fxaa_quality_edge_threshold — Min edge contrast to trigger AA [0.063, 0.333].
 *   fxaa_quality_edge_threshold_min — Dark area threshold.  0.0312 default.
 */

uniform sampler2D color_tx;

uniform float fxaa_quality_subpix              = 0.75;
uniform float fxaa_quality_edge_threshold      = 0.166;
uniform float fxaa_quality_edge_threshold_min  = 0.0312;

out vec4 out_color;

/* ------------------------------------------------------------------ */
/* Luma helper
 *
 * In HDR linear space, luma must use the correct Rec.709 coefficients.
 * The sqrt approximation used in SDR FXAA (luma = sqrt(dot(c, luma_coef)))
 * is wrong here because it assumes gamma-2.0 encoding. */
float rgb_to_luma(vec3 c)
{
  return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

/* Luminance-safe sample: returns luma directly from the texture fetch.
 * Inlining this avoids re-computing luma for the 4 neighbour taps. */
float luma_at(sampler2D tex, vec2 uv)
{
  return rgb_to_luma(texture(tex, uv).rgb);
}

/* ================================================================== */
/* FXAA 3.11 (Quality preset 25, adapted for HDR linear)             */
/* ================================================================== */

void main()
{
  vec2 inv_res = 1.0 / vec2(textureSize(color_tx, 0));
  vec2 uv      = gl_FragCoord.xy * inv_res;

  /* ---- Luma at the current pixel and its 4 cardinal neighbours ---- */
  float luma_m  = luma_at(color_tx, uv);
  float luma_n  = luma_at(color_tx, uv + vec2( 0.0,  inv_res.y));
  float luma_s  = luma_at(color_tx, uv + vec2( 0.0, -inv_res.y));
  float luma_e  = luma_at(color_tx, uv + vec2( inv_res.x, 0.0));
  float luma_w  = luma_at(color_tx, uv + vec2(-inv_res.x, 0.0));

  float range_max = max(max(luma_n, luma_s), max(luma_e, max(luma_w, luma_m)));
  float range_min = min(min(luma_n, luma_s), min(luma_e, min(luma_w, luma_m)));
  float range     = range_max - range_min;

  /* Early-out: if contrast is below threshold, skip anti-aliasing.
   * This is the most expensive pixel's "free" no-op. */
  if (range < max(fxaa_quality_edge_threshold_min, range_max * fxaa_quality_edge_threshold)) {
    out_color = texture(color_tx, uv);
    return;
  }

  /* ---- Diagonal neighbours for edge direction ---- */
  float luma_nw = luma_at(color_tx, uv + vec2(-inv_res.x,  inv_res.y));
  float luma_ne = luma_at(color_tx, uv + vec2( inv_res.x,  inv_res.y));
  float luma_sw = luma_at(color_tx, uv + vec2(-inv_res.x, -inv_res.y));
  float luma_se = luma_at(color_tx, uv + vec2( inv_res.x, -inv_res.y));

  /* ---- Sub-pixel blend factor ----
   * Computes a 3×3 weighted luma average and measures how much the centre
   * pixel deviates from it.  A high deviation means it is an isolated
   * thin feature (sub-pixel aliasing) and deserves extra blending. */
  float luma_nswe    = luma_n + luma_s + luma_e + luma_w;
  float luma_corners = luma_nw + luma_ne + luma_sw + luma_se;
  float luma_average = (luma_nswe * 2.0 + luma_corners) / 12.0;
  float sub_pixel_offset_raw = clamp(abs(luma_average - luma_m) / range, 0.0, 1.0);
  float sub_pixel_offset     = sub_pixel_offset_raw * sub_pixel_offset_raw *
                               fxaa_quality_subpix;

  /* ---- Edge orientation ---- */
  /* Sobel-like gradient estimator; larger Gx vs Gy → horizontal edge. */
  float edge_h = abs(-2.0 * luma_w + luma_nw + luma_sw) +
                 abs(-2.0 * luma_m + luma_n  + luma_s ) * 2.0 +
                 abs(-2.0 * luma_e + luma_ne + luma_se);
  float edge_v = abs(-2.0 * luma_n + luma_nw + luma_ne) +
                 abs(-2.0 * luma_m + luma_w  + luma_e ) * 2.0 +
                 abs(-2.0 * luma_s + luma_sw + luma_se);

  bool is_horizontal = (edge_h >= edge_v);

  /* ---- Step direction (perpendicular to the edge) ---- */
  float step_len = is_horizontal ? inv_res.y : inv_res.x;

  float luma_pos  = is_horizontal ? luma_n : luma_e;
  float luma_neg  = is_horizontal ? luma_s : luma_w;
  float gradient_pos = abs(luma_pos - luma_m);
  float gradient_neg = abs(luma_neg - luma_m);

  bool step_positive = (gradient_pos >= gradient_neg);
  float step_sign    = step_positive ? 1.0 : -1.0;

  vec2 step_uv = is_horizontal ? vec2(0.0, step_sign * step_len) :
                                  vec2(step_sign * step_len, 0.0);

  /* Average luma across the edge. */
  float luma_local_avg = 0.5 * (luma_m + (step_positive ? luma_pos : luma_neg));
  float gradient_scaled = 0.25 * max(gradient_pos, gradient_neg);

  /* ---- Iterative end-of-edge search (quality preset 25: 12 iterations) ---- */
  vec2  uv_p        = uv + step_uv * 0.5;
  vec2  uv_n        = uv - step_uv * 0.5;
  vec2  edge_step   = is_horizontal ? vec2(inv_res.x, 0.0) : vec2(0.0, inv_res.y);

  bool done_p = false, done_n = false;
  float luma_end_p = 0.0, luma_end_n = 0.0;

  /* Quality preset 25 step offsets: 1,2,2,2,2,4,4 (10 total, good quality). */
  const float STEPS[10] = float[10](1.0,1.0,1.0,1.0,1.0,1.5,2.0,2.0,2.0,4.0);

  for (int i = 0; i < 10; i++) {
    float s = STEPS[i];
    if (!done_p) {
      luma_end_p = luma_at(color_tx, uv_p + edge_step * s);
      done_p     = abs(luma_end_p - luma_local_avg) >= gradient_scaled;
      uv_p      += edge_step * s;
    }
    if (!done_n) {
      luma_end_n = luma_at(color_tx, uv_n - edge_step * s);
      done_n     = abs(luma_end_n - luma_local_avg) >= gradient_scaled;
      uv_n      -= edge_step * s;
    }
    if (done_p && done_n) {
      break;
    }
  }

  /* ---- Compute blend amount from edge position ---- */
  float dist_p = is_horizontal ? abs(uv_p.x - uv.x) : abs(uv_p.y - uv.y);
  float dist_n = is_horizontal ? abs(uv_n.x - uv.x) : abs(uv_n.y - uv.y);

  bool is_near_pos = (dist_p <= dist_n);
  float span       = dist_p + dist_n;
  float pixel_offset = -( is_near_pos ? dist_p : dist_n) / span + 0.5;

  /* Reject offsets that would blend in the wrong direction. */
  bool luma_m_smaller = (luma_m < luma_local_avg);
  bool correct_variation = (( is_near_pos ? (luma_end_p - luma_local_avg) :
                                            (luma_end_n - luma_local_avg)) < 0.0)
                          != luma_m_smaller;

  float final_offset = max(correct_variation ? pixel_offset : 0.0, sub_pixel_offset);

  /* ---- Apply blend ---- */
  vec2 final_uv = uv;
  if (is_horizontal) {
    final_uv.y += final_offset * step_sign * step_len;
  }
  else {
    final_uv.x += final_offset * step_sign * step_len;
  }

  out_color = texture(color_tx, final_uv);
  /* Preserve the original alpha — FXAA blending should never modify alpha. */
  out_color.a = texture(color_tx, uv).a;
}
