/* SPDX-FileCopyrightText: 2024 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Deterministic 3x3 PCF Shadow Filter — Compute Shader
 *
 * This runs as a full-screen compute dispatch after the G-Buffer fill and before
 * the deferred lighting pass. It writes one float (shadow factor [0,1]) per pixel
 * into shadow_mask_img, which the lighting pass samples as a plain texture.
 *
 * Why compute, not fragment:
 *   Each pixel has its own world-space position and light-space coordinate derived
 *   from the depth buffer. These cannot be passed as interpolated varyings from a
 *   vertex shader (that is only valid for geometry shading, not for a deferred filter).
 *   A compute shader reconstructs position from depth per-thread, exactly like
 *   GTAO, SSGI, and SSR do in the same pipeline.
 *
 * Why PCF 3x3 (9 taps) instead of EEVEE's stochastic PCSS:
 *   EEVEE's PCSS uses random-rotated Poisson disk samples and requires TAA or
 *   a denoiser to converge. In game mode there is no TAA history — we get one
 *   sample per pixel per frame. 9 deterministic taps in a grid give stable,
 *   noise-free soft shadows with zero denoising cost.
 *
 * Contact hardening:
 *   The filter radius grows with the blocker depth relative to the receiver.
 *   This approximates the penumbra growth of a real area light:
 *     penumbra_width ≈ light_size * (receiver_depth - blocker_depth) / blocker_depth
 *   We clamp to [1, MAX_PCF_RADIUS] texels to bound cost and avoid over-blurring
 *   on geometry that is far from any occluder.
 *
 * Slope-scaled bias:
 *   Normal bias is added in the normal direction before projecting to light space,
 *   which eliminates shadow acne on steep surfaces without over-darkening flat ones.
 *   The bias magnitude is proportional to (1 - dot(N, L)), clamped to [min, max].
 *
 * Layout (must match eevee_game_shadow_pcf_compute ShaderCreateInfo):
 *   sampler 0 : depth_tx       — scene depth, R32F or D32F
 *   gbuffer   : world-space normals, decoded with eevee_gbuffer_read_lib.glsl
 *   sampler 2 : shadow_atlas   — fixed 4096x4096 depth atlas, D32F
 *   image   0 : shadow_mask    — output RGBA16F (R = shadow factor, GBA unused)
 *   UBO/SSBO  : shadow_data    — ShadowUniformData (cascade matrices, splits, bias)
 *   UBO/SSBO  : uniform_data   — per-frame camera matrices (viewprojinv for reconstruct)
 *
 * Local group 8x8 = 64 threads: fills one NVIDIA warp-pair or one AMD wavefront.
 */

#include "infos/eevee_game_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_game_shadow_pcf_compute)

#include "eevee_reverse_z_lib.glsl"
#include "eevee_gbuffer_read_lib.glsl"

/* ------------------------------------------------------------------ */
/* Inputs                                                               */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

/* Maximum PCF filter radius in shadow-map texels.
 * 8 texels at 4096 resolution = 8/4096 = 0.002 of the atlas — large enough
 * for soft penumbrae without exceeding a single tile boundary (1024 texels). */
#define MAX_PCF_RADIUS 8.0

/* ------------------------------------------------------------------ */
/* World-space position reconstruction from depth
 *
 * Standard technique: unproject the NDC position using the inverse VP matrix.
 * All deferred effects (GTAO, SSR, SSGI) use the same reconstruction path. */

/* ------------------------------------------------------------------ */
/* Cascade selection
 *
 * Returns the cascade index [0, 3] whose split depth covers the fragment.
 * cascade_splits.xyzw stores the camera-space far depth of cascades 0..3.
 * Fragments beyond the last cascade return -1 (no shadow). */
int select_cascade(float view_depth)
{
  if (view_depth < cascade_splits.x) {
    return 0;
  }
  if (view_depth < cascade_splits.y) {
    return 1;
  }
  if (view_depth < cascade_splits.z) {
    return 2;
  }
  if (view_depth < cascade_splits.w) {
    return 3;
  }
  return -1; /* Beyond shadow distance. */
}

/* ------------------------------------------------------------------ */
/* Slope-scaled normal bias
 *
 * Move the sample point along the surface normal before projecting to
 * light space. The amount scales with (1 - N·L) so steep surfaces get
 * more bias and front-facing surfaces get almost none.
 *
 * This is preferable to a constant depth bias because it adapts to surface
 * angle without over-darkening flat geometry lit from above.
 *
 * bias_scale is shadow_bias (world units) * (1 - N·L), clamped to [0, 4x]. */
vec3 apply_normal_bias(vec3 world_pos, vec3 world_normal, vec3 light_dir)
{
  float n_dot_l     = max(dot(world_normal, light_dir), 0.0);
  float slope_scale = 1.0 - n_dot_l;               /* 0 when perpendicular, 1 when parallel */
  float bias        = shadow_bias * (0.5 + slope_scale * 3.5); /* [0.5x, 4x] range */
  return world_pos + world_normal * bias;
}

/* ------------------------------------------------------------------ */
/* Contact-hardening PCF 3x3
 *
 * Performs a 3x3 grid of hardware shadow comparisons centred on uv.
 * The grid spacing (pcf_radius in texels) controls softness.
 *
 * Hardware shadow comparison (sampler2DShadow + texture()) performs bilinear
 * interpolation of the depth comparison result, giving smooth sub-texel
 * transitions for free. The 3x3 grid extends this over a wider area.
 *
 * contact_hardening_radius:
 *   The blocker search is a 1-tap depth read at the centre. If the blocker
 *   depth is close to the receiver depth (hard contact) the radius collapses
 *   to 1 texel (sharp shadow). If it is far away (soft penumbra) the radius
 *   grows toward MAX_PCF_RADIUS.
 *
 *   penumbra ≈ light_size * (d_receiver - d_blocker) / d_blocker
 *   We use a fixed light_size of 1.0 (normalised) and scale by pcf_offset_scale. */
float pcf_3x3(sampler2DShadow atlas, vec3 shadow_coord, vec2 tile_min, vec2 tile_max,
              float pcf_radius)
{
  vec2 texel_size = 1.0 / vec2(shadow_map_res);
  vec2 offset     = texel_size * pcf_radius;

  float sum = 0.0;
  for (int y = -1; y <= 1; y++) {
    for (int x = -1; x <= 1; x++) {
      /* Offset only XY; Z (depth) is the same for all taps — the comparison
       * value does not change with filter position, only the occluder sample. */
      vec2 tap_uv  = shadow_coord.xy + vec2(float(x), float(y)) * offset;
      /* Taps must not leak into the neighbouring cascade tile. */
      tap_uv = clamp(tap_uv, tile_min, tile_max);
      /* The compare is `ref <= stored` (LEQUAL, fixed by the GPU module).
       * With reversed depth the stored value is the nearest surface to the
       * light, so the compare is true when something at least as near as the
       * receiver is there: that is the occluded case. The normal bias moves
       * the receiver toward the light, past its own stored depth. */
      sum += 1.0 - texture(atlas, vec3(tap_uv, shadow_coord.z));
    }
  }
  return sum / 9.0;
}

/* ------------------------------------------------------------------ */
/* Lit fraction of a world position in one cascade: 1 outside its tile. */
float cascade_shadow(int cascade_idx, vec3 world_pos, vec3 world_normal)
{
  mat4 lvp = (cascade_idx == 0) ? cascade_viewproj_0 :
             (cascade_idx == 1) ? cascade_viewproj_1 :
             (cascade_idx == 2) ? cascade_viewproj_2 : cascade_viewproj_3;

  /* Direction toward the light, for N.L: the light's +Z axis in world space.
   * It is the third row of the cascade matrix up to the orthographic depth
   * scale, which is negative, hence the sign flip. */
  vec3 light_dir = normalize(vec3(-lvp[0][2], -lvp[1][2], -lvp[2][2]));

  /* Apply normal bias to the world position before projecting to light space. */
  vec3 biased_pos = apply_normal_bias(world_pos, world_normal, light_dir);

  /* Project biased world position into shadow-atlas UV space. */
  vec4 light_clip  = lvp * vec4(biased_pos, 1.0);
  vec3 shadow_coord = light_clip.xyz / light_clip.w;
  shadow_coord      = shadow_coord * 0.5 + 0.5; /* NDC [-1,1] → UV [0,1] */

  /* Reject samples outside the cascade tile boundary. */
  if (any(lessThan(shadow_coord.xy, vec2(0.0))) ||
      any(greaterThan(shadow_coord.xy, vec2(1.0))))
  {
    return 1.0;
  }

  /* Tile-local [0,1] to atlas UV: the cascades are the bottom row of the
   * 4x4 tile grid, cascade i in column i. */
  vec2 tile_min = vec2(float(cascade_idx), 0.0) * 0.25;
  vec2 half_texel = vec2(0.5) / vec2(shadow_map_res);
  vec3 atlas_coord;
  atlas_coord.xy = tile_min + shadow_coord.xy * 0.25;
  /* The atlas stores reversed depth (1 = near the light): reverse the
   * reference to compare in the same space. */
  atlas_coord.z = 1.0 - shadow_coord.z;

  /* Fixed filter radius driven by pcf_offset_scale (1.0 = 2 texels). */
  float pcf_radius = clamp(pcf_offset_scale * 2.0, 1.0, MAX_PCF_RADIUS);

  return pcf_3x3(shadow_atlas,
                 atlas_coord,
                 tile_min + half_texel,
                 tile_min + vec2(0.25) - half_texel,
                 pcf_radius);
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

void main()
{
  const ivec2 px     = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 res    = imageSize(shadow_mask_img);

  if (any(greaterThanEqual(px, res))) {
    return;
  }

  const vec2 uv = (vec2(px) + 0.5) / vec2(res);

  /* Early-out: sky pixels carry depth = 1.0 (far plane).
   * They receive no shadow; write 1.0 (fully lit) and exit. */
  float depth = reverse_z::read(texture(depth_tx, uv).r);
  if (depth >= 0.9999) {
    imageStore(shadow_mask_img, px, vec4(1.0));
    return;
  }

  /* Reconstruct world-space position. Unprojecting an NDC point gives
   * (world, 1) / clip.w, so the homogeneous w is the reciprocal of clip.w,
   * and for a perspective camera clip.w is the view-space depth. The split
   * distances are view-space depths too, so cascade selection costs nothing
   * beyond the unprojection that is needed anyway. */
  vec4 world_h = viewprojinv * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
  vec3 world_pos = world_h.xyz / world_h.w;
  float view_depth = 1.0 / world_h.w;

  /* Shading normal of the first closure, decoded from EEVEE's G-Buffer.
   * A pixel can have depth and still no closure (emission only surface): its
   * normal layer was never written, and decoding garbage would spread NaNs
   * through the filters downstream. */
  ivec2 gbuf_texel = px;
  if (gbuffer::read_header(gbuf_texel).is_empty()) {
    imageStore(shadow_mask_img, px, vec4(1.0));
    return;
  }
  vec3 world_normal = gbuffer::read_normal(gbuf_texel);

  int cascade_idx = select_cascade(view_depth);
  if (cascade_idx < 0) {
    imageStore(shadow_mask_img, px, vec4(1.0)); /* Beyond shadow range. */
    return;
  }

  float shadow_factor = cascade_shadow(cascade_idx, world_pos, world_normal);
  /* Over the end of its range a cascade fades into the next one, the last
   * into no shadow: the texel size jumps by the split ratio there, and a seam
   * shows where a blend does not. Two lookups in the band only; each cascade
   * is fitted to also cover the band of the previous one. */
  float split = cascade_splits[cascade_idx];
  float blend = clamp((view_depth / split - (1.0 - SHADOW_CASCADE_BLEND)) *
                          (1.0 / SHADOW_CASCADE_BLEND),
                      0.0,
                      1.0);
  if (blend > 0.0) {
    float next = (cascade_idx < 3) ? cascade_shadow(cascade_idx + 1, world_pos, world_normal) :
                                     1.0;
    shadow_factor = mix(shadow_factor, next, blend);
  }

  imageStore(shadow_mask_img, px, vec4(shadow_factor));
}
