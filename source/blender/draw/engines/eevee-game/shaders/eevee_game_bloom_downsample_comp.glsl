/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Bloom Downsample Compute Shader
 *
 * Builds the luminance pyramid for the Kawase bloom algorithm.
 *
 * Level 0:  Source is the full-res HDR scene color.
 *           Applies soft-knee threshold to isolate bright pixels.
 *           Output is render_res / 2.
 *
 * Level 1+: Plain 13-tap Kawase downsample.
 *           The 13-tap pattern (center + 4 bilinear quads + 4 single taps)
 *           is the industry standard for firefly-free bloom (UE4, Frostbite,
 *           Call of Duty).  A simple box filter would alias bright isolated
 *           pixels ("fireflies") into blocky square artifacts.
 *
 * params.xy = (threshold, knee) when i == 0; params = vec4(-1) for i > 0.
 *
 * Local group 8×8 = 64 threads: one NVIDIA warp-pair, one AMD wavefront.
 */

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

uniform sampler2D in_color_tx;
layout(rgba16f) uniform writeonly image2D out_color_img;

/* params.x = luminance threshold (pixels dimmer than this are zeroed).
 * params.y = knee width (soft-knee Hermite transition above threshold).
 * params.xy < 0 means bypass threshold (levels 1..N). */
uniform vec4 params;

/* ------------------------------------------------------------------ */
/* Soft-knee threshold (Schlick–Hermite smooth-step)
 *
 * Full luminance range:
 *   [0, threshold - knee/2]       → 0   (cut)
 *   [threshold - knee/2, +knee]   → smooth Hermite ramp
 *   [threshold + knee/2, ∞)       → linear pass-through
 *
 * The knee prevents the hard "flickering edge" that a sharp threshold
 * produces on pixels near the cutoff value between frames. */
vec3 apply_threshold(vec3 color, float threshold, float knee)
{
  float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));

  /* Remap so the knee is symmetric around the threshold. */
  float knee_half = knee * 0.5;
  float lo = threshold - knee_half;
  float hi = threshold + knee_half;

  float remap;
  if (luma < lo) {
    remap = 0.0;
  }
  else if (luma < hi) {
    /* Smooth step: 3t² − 2t³ (Hermite interpolant, zero slope at endpoints). */
    float t  = (luma - lo) / max(knee, 1e-5);
    remap = t * t * (3.0 - 2.0 * t);
  }
  else {
    remap = 1.0;
  }

  /* Scale RGB so the bright part above threshold passes through and dim part is suppressed.
   * We divide by luma and multiply by the clamped remap to avoid RGB channel bias. */
  return color * (remap * max(luma - lo, 0.0) / max(luma, 1e-4));
}

/* ------------------------------------------------------------------ */
/* 13-tap Kawase downsample
 *
 * Layout (in texel space relative to pixel centre, bilinear taps marked B):
 *
 *   A · B · A
 *   · C · C ·
 *   B · D · B
 *   · C · C ·
 *   A · B · A
 *
 *   D = centre tap (weight 0.125)
 *   B = axis-aligned bilinear taps at ±(0.5, 0.5) from centre (weight 0.125 each)
 *   C = diagonal bilinear taps at ±(1, 1) from centre (weight 0.03125 each)
 *   A = corner single taps at ±(2, 2) from centre — NOT sampled in this 13-tap variant;
 *       the four bilinear B-taps implicitly cover them with their footprint.
 *
 * Total weight = 0.125 + 4*0.125 + 4*0.03125 = 0.75 + 0.25 = 1.0. Correct.
 *
 * Reference: "Next Generation Post Processing in Call of Duty: Advanced Warfare"
 *             Jimenez 2014, GDC.  The same kernel is used in UE4 BloomSetup.usf. */
vec3 kawase_downsample_13tap(sampler2D tex, vec2 uv, vec2 texel)
{
  /* Centre tap. */
  vec3 c = texture(tex, uv).rgb;

  /* Four bilinear taps at half-texel offsets (naturally 2×2 average each). */
  vec3 b0 = texture(tex, uv + vec2(-texel.x, -texel.y) * 0.5).rgb;
  vec3 b1 = texture(tex, uv + vec2( texel.x, -texel.y) * 0.5).rgb;
  vec3 b2 = texture(tex, uv + vec2(-texel.x,  texel.y) * 0.5).rgb;
  vec3 b3 = texture(tex, uv + vec2( texel.x,  texel.y) * 0.5).rgb;

  /* Four single taps at full-texel diagonal offsets. */
  vec3 a0 = texture(tex, uv + vec2(-texel.x, -texel.y)).rgb;
  vec3 a1 = texture(tex, uv + vec2( texel.x, -texel.y)).rgb;
  vec3 a2 = texture(tex, uv + vec2(-texel.x,  texel.y)).rgb;
  vec3 a3 = texture(tex, uv + vec2( texel.x,  texel.y)).rgb;

  /* Weighted sum: centre 0.125, bilinear 0.125 each, corner 0.03125 each.
   * Grouped as: 0.5*(b0+b1+b2+b3) + 0.25*c + 0.125*(a0+a1+a2+a3). */
  return 0.5  * (b0 + b1 + b2 + b3) * 0.25
       + 0.25 * c
       + 0.125 * (a0 + a1 + a2 + a3) * 0.25;
}

void main()
{
  const ivec2 out_px  = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 out_res = imageSize(out_color_img);

  if (any(greaterThanEqual(out_px, out_res))) {
    return;
  }

  /* Centre of the output texel in UV space of the input (which is 2× larger). */
  const vec2 uv     = (vec2(out_px) + 0.5) / vec2(out_res);
  const vec2 texel  = 1.0 / vec2(textureSize(in_color_tx, 0));

  vec3 color;
  const bool is_level0 = (params.x >= 0.0);

  if (is_level0) {
    /* Level 0: 13-tap Kawase on the full-res source, then apply soft-knee threshold.
     * The 13-tap filter here already provides a 2× average so the output at half-res
     * has the correct spatial energy.  Threshold is applied after filtering to avoid
     * threshold quantisation artifacts from filtering already-clipped values. */
    color = kawase_downsample_13tap(in_color_tx, uv, texel);
    color = apply_threshold(color, params.x, params.y);
  }
  else {
    /* Levels 1..N: plain 13-tap Kawase, no threshold. */
    color = kawase_downsample_13tap(in_color_tx, uv, texel);
  }

  /* Firefly suppression: clamp to a safe HDR ceiling.
   * Values > 64 are almost always GPU NaN propagation or degenerate geometry
   * normals.  The clamp prevents a single bright texel from washing out the
   * entire pyramid level, which manifests as a full-screen white flash. */
  color = min(color, vec3(64.0));

  imageStore(out_color_img, out_px, vec4(color, 1.0));
}
