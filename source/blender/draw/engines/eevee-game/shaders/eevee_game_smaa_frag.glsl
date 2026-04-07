/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * SMAA — Subpixel Morphological Anti-Aliasing
 *
 * Three-pass fragment shader for eevee_game.
 * Controlled by the SMAA_STAGE preprocessor define (set by ShaderCreateInfo).
 *
 *   SMAA_STAGE == 0  EDGE DETECTION
 *     Input:  color_tx (HDR linear RGBA16F after tonemapping to [0,1] LDR).
 *     Output: RG8 edge texture.
 *     Method: Luma-based edge detection (faster than colour-based; equivalent
 *             quality for AA purposes since luma dominates perceived edges).
 *             Luma computed in linear space with Rec.709 coefficients.
 *
 *   SMAA_STAGE == 1  BLENDING WEIGHT CALCULATION
 *     Input:  edges_tx (from stage 0), area_tx, search_tx (look-up tables).
 *     Output: RGBA8 blend weight texture.
 *     SMAA_MAX_SEARCH_STEPS 16 gives good quality at reasonable cost.
 *
 *   SMAA_STAGE == 2  NEIGHBOURHOOD BLENDING
 *     Input:  color_tx, blend_tx (from stage 1).
 *     Output: Anti-aliased colour.
 *     Note: We operate in linear HDR space (not log or gamma) because
 *           eevee_game's SMAA runs before tonemapping or immediately after
 *           FSR output.  Linear blending is correct for HDR content.
 *
 * SMAA configuration:
 *   SMAA_PRESET_HIGH is used (SMAA_MAX_SEARCH_STEPS=16, SMAA_MAX_SEARCH_STEPS_DIAG=8).
 *   For lower-end targets, SMAA_PRESET_MEDIUM (steps=8) can be substituted.
 *
 * Integration notes:
 *   - The vertex shader (eevee_game_smaa_vert.glsl) sets up uvs, pixcoord,
 *     and the offset arrays required by the SMAA lib.
 *   - area_tx and search_tx are static look-up tables loaded once at init.
 *   - The SMAA library header is included from gpu_shader_smaa_lib.glsl
 *     (the same version Workbench uses — no modifications needed).
 */

/* ------------------------------------------------------------------ */
/* SMAA configuration — must precede the library include.             */
/* ------------------------------------------------------------------ */

#define SMAA_GLSL_4
#define SMAA_PRESET_HIGH
/* Enable diagonal pattern detection for better diagonal edge quality.
 * Small cost: ~5% more ALU in the weight pass. */
#define SMAA_DISABLE_DIAG_DETECTION 0

/* Force luma edge detection (fastest and sufficient for AA). */
#undef SMAA_EDGE_DETECTION_MODE
#define SMAA_EDGE_DETECTION_MODE 1  /* 1 = luma, 2 = colour, 3 = depth */

/* RT metrics injected by ShaderCreateInfo via a push constant.
 * SMAA_RT_METRICS = vec4(1/W, 1/H, W, H). */
uniform vec4 smaa_rt_metrics;
#define SMAA_RT_METRICS smaa_rt_metrics

/* ---- Luma helper for SMAA luma edge detection ----
 *
 * SMAA's luma detection expects a pre-computed luma value per texel.
 * In HDR linear space we use standard Rec.709 weights.
 * Note: the SMAA library's SMAALumaEdgeDetectionPS expects the colour texture
 * to have luma in the alpha channel, OR overrides with a custom macro.
 * We inject luma via a wrapper that reads RGB and returns luma in alpha. */

/* Custom luma sample: reads the texture and returns (rgb, luma) as vec4.
 * SMAA uses texture().a as the luma in luma-detection mode. */
vec4 smaa_luma_sample(sampler2D tex, vec2 uv)
{
  vec4 c    = texture(tex, uv);
  float luma = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
  return vec4(c.rgb, luma);
}

/* Override the texture fetch macro that SMAA uses internally.
 * This injects the luma-in-alpha trick without modifying the library. */
#define SMAATexture2D(tex)        sampler2D tex
#define SMAATexturePass2(tex)     tex
#define SMAASampleLevelZero(tex, coord)       smaa_luma_sample(tex, coord)
#define SMAASampleLevelZeroOffset(tex, coord, offset) \
    smaa_luma_sample(tex, coord + vec2(offset) * smaa_rt_metrics.xy)

/* ------------------------------------------------------------------ */
#include "gpu_shader_smaa_lib.glsl"
/* ------------------------------------------------------------------ */

/* ---- Per-stage varyings (set by vertex shader) ---- */
in vec2 uvs;
in vec2 pixcoord;
in vec4 offset[3];

/* ---- Stage-specific uniforms ---- */
#if SMAA_STAGE == 0
  uniform sampler2D color_tx;
  out vec2 out_edges;

#elif SMAA_STAGE == 1
  uniform sampler2D edges_tx;
  uniform sampler2D area_tx;
  uniform sampler2D search_tx;
  out vec4 out_weights;

#elif SMAA_STAGE == 2
  uniform sampler2D color_tx;
  uniform sampler2D blend_tx;
  out vec4 out_color;

#endif

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

void main()
{
#if SMAA_STAGE == 0
  /* ---- Edge Detection ---- */
  out_edges = SMAALumaEdgeDetectionPS(uvs, offset, color_tx);

  /* Discard pixels with no edges to save the weight-pass bandwidth.
   * gpu_discard_fragment() is used rather than `discard` for compatibility
   * with the Blender GPU backend (Metal, Vulkan do not support bare `discard`
   * in a well-defined way without early fragment tests). */
  if (dot(out_edges, vec2(1.0)) == 0.0) {
    gpu_discard_fragment();
    return;
  }

#elif SMAA_STAGE == 1
  /* ---- Blend Weight Calculation ---- */
  out_weights = SMAABlendingWeightCalculationPS(
      uvs, pixcoord, offset,
      edges_tx, area_tx, search_tx,
      /* subsample_indices */ vec4(0.0));

#elif SMAA_STAGE == 2
  /* ---- Neighbourhood Blending ---- */
  /* Linear HDR blending: SMAA operates in whatever colour space the input
   * is in.  In eevee_game this pass runs after tonemapping on LDR [0,1]
   * data for stage 2.  The linear blend is correct in that space. */
  out_color = SMAANeighborhoodBlendingPS(uvs, offset[0], color_tx, blend_tx);

  /* Clamp negative values that can arise from the bilinear weight blending
   * in HDR content (theoretically impossible in LDR but guard anyway). */
  out_color.rgb = max(out_color.rgb, vec3(0.0));

  /* Normalise alpha: game overlays expect either 0 (transparent) or 1 (opaque).
   * SMAA blending can produce fractional alpha near geometry edges; force it. */
  out_color.a = (out_color.a > 0.5) ? 1.0 : 0.0;

#endif
}
