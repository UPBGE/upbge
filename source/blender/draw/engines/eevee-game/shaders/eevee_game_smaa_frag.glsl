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
/* SMAA                                                                 */
/* ------------------------------------------------------------------ */

#include "infos/eevee_game_infos.hh"

/* Adjust according to SMAA_STAGE for C++ compilation. */
FRAGMENT_SHADER_CREATE_INFO(eevee_game_smaa_weight)

/* SMAA_GLSL_3, SMAA_PRESET_HIGH, SMAA_RT_METRICS and SMAA_LUMA_WEIGHT come
 * from the create info. The luma weights are Rec.709: the input is scene
 * linear HDR, and edges are found on luma, the cheapest mode and enough for
 * geometric aliasing. */
#include "gpu_shader_smaa_lib.glsl"

void main()
{
  /* Varyings cannot be arrays. */
  float4 offset[3];
  offset[0] = offset0;
  offset[1] = offset1;
  offset[2] = offset2;

#if SMAA_STAGE == 0
  /* ---- Edge Detection ---- */
  out_edges = SMAALumaEdgeDetectionPS(uvs, offset, color_tx);

  /* Discard pixels with no edges to save the weight-pass bandwidth.
   * gpu_discard_fragment() is used rather than `discard` for compatibility
   * with the Blender GPU backend (Metal, Vulkan do not support bare `discard`
   * in a well-defined way without early fragment tests). */
  if (dot(out_edges, float2(1.0)) == 0.0) {
    gpu_discard_fragment();
    return;
  }

#elif SMAA_STAGE == 1
  /* ---- Blend Weight Calculation ---- */
  out_weights = SMAABlendingWeightCalculationPS(
      uvs, pixcoord, offset,
      edges_tx, area_tx, search_tx,
      /* subsample_indices */ float4(0.0));

#elif SMAA_STAGE == 2
  /* ---- Neighbourhood Blending ---- */
  /* Linear HDR blending: SMAA operates in whatever colour space the input
   * is in.  In eevee_game this pass runs after tonemapping on LDR [0,1]
   * data for stage 2.  The linear blend is correct in that space. */
  out_color = SMAANeighborhoodBlendingPS(uvs, offset[0], color_tx, blend_tx);

  /* Clamp negative values that can arise from the bilinear weight blending
   * in HDR content (theoretically impossible in LDR but guard anyway). */
  out_color.rgb = max(out_color.rgb, float3(0.0));

  /* Normalise alpha: game overlays expect either 0 (transparent) or 1 (opaque).
   * SMAA blending can produce fractional alpha near geometry edges; force it. */
  out_color.a = (out_color.a > 0.5) ? 1.0 : 0.0;

#endif
}
