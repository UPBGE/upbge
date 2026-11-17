/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Volumetric Scatter — Froxel Light Injection
 *
 * Pass 1 of the two-pass volumetric pipeline.
 *
 * Each thread processes one froxel (frustum-aligned voxel) and computes how
 * much in-scattered light reaches it from all directional lights.
 *
 * Froxel grid layout:
 *   X, Y: screen tiles of tile_size pixels
 *   Z:    exponentially-spaced depth slices (denser near camera)
 *   Exponential Z gives higher sample density near the camera where volumetric
 *   detail matters most (fog near the ground, godrays) and coarser sampling
 *   in the far field where the effect integrates to a smooth average anyway.
 *
 * Output: RGBA16F froxel grid.
 *   RGB = in-scattered radiance (L_i × σ_s)
 *   A   = extinction coefficient (σ_t = σ_s + σ_a)
 *
 * Phase function: Henyey-Greenstein (single-parameter).
 *   HG(cosθ, g) = (1 - g²) / (4π * (1 + g² - 2g·cosθ)^(3/2))
 *   g=0: isotropic; g>0: forward-scattering (fog); g<0: back-scattering.
 *
 * Shadow lookup: samples the static CSM shadow atlas (cascade 0 = nearest).
 * Only directional lights are handled here; punctual shadows are read from
 * shadow_mask_tx in the resolve pass.
 *
 * Dispatch: ceil(grid_x/8) × ceil(grid_y/8) × 1.
 * The Z dimension is the shader's inner loop (cache-friendly sequential read
 * along the depth axis — all Z slices share the same XY screen tile data).
 *
 * Local group 8×8×1.
 */

#include "infos/eevee_game_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_game_volume_scatter)

#include "eevee_game_light_lib.glsl"

/* One entry per froxel:
 *   out_grid_img       RGB = sigma_s * L_in + emission
 *   out_extinction_img RGB = sigma_t = sigma_s + sigma_a, per channel
 *
 * The lights are those of the screen tiles the froxel column covers, from
 * the tiled light culling of the surfaces. */

/* ------------------------------------------------------------------ */
/* Henyey-Greenstein phase function
 *
 * Gives the probability density of scattering light from direction L
 * into direction V.  cos_theta = dot(-V, L).
 * Result is normalised to integrate to 1 over the sphere. */
float hg_phase(float cos_theta, float g)
{
  float g2   = g * g;
  float denom = 1.0 + g2 - 2.0 * g * cos_theta;
  return (1.0 - g2) / (12.566370 * pow(max(denom, 1e-4), 1.5));
}

/* ------------------------------------------------------------------ */
/* Froxel world-space position reconstruction
 *
 * Exponential Z spacing: the normalized slice coordinate w maps to the view
 * depth d = z_near * (z_far / z_near)^w, as in the integration pass.
 *
 * Along the column of a screen position the world position is linear in d,
 * for both projections: origin + ray * d. Two unprojections per column, at
 * the clip planes (NDC -1 and 1 are depth z_near and z_far), replace one per
 * froxel; in perspective origin is the camera. */
void froxel_column(vec2 uv, out vec3 origin, out vec3 ray)
{
  vec2 ndc = uv * 2.0 - 1.0;
  vec4 p_near = viewprojinv * vec4(ndc, -1.0, 1.0);
  vec4 p_far  = viewprojinv * vec4(ndc, 1.0, 1.0);
  vec3 near_ws = p_near.xyz / p_near.w;
  vec3 far_ws  = p_far.xyz / p_far.w;
  ray    = (far_ws - near_ws) / (z_far - z_near);
  origin = near_ws - ray * z_near;
}

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

void main()
{
  /* XY dispatch; Z is the inner loop. */
  const ivec2 froxel_xy = ivec2(gl_GlobalInvocationID.xy);

  /* Grid dimensions in froxels. */
  const ivec3 grid_size = imageSize(out_grid_img);

  if (any(greaterThanEqual(froxel_xy, grid_size.xy))) {
    return;
  }

  /* Light tiles under this column. The grid splits the render extent in
   * ceil(extent / 16) cells, a bit under the 16 px of a light tile, so a
   * column straddles at most 2x2 tiles: the union of their masks. The light
   * culling slab of a tile ends at its farthest opaque surface, behind which
   * no froxel is ever seen. */
  ivec2 extent = game_lighting_buf.render_extent;
  ivec2 px_lo = (froxel_xy * extent) / grid_size.xy;
  ivec2 px_hi = ((froxel_xy + 1) * extent + grid_size.xy - 1) / grid_size.xy - 1;
  ivec2 tile_lo = min(px_lo / LIGHT_TILE_SIZE, game_lighting_buf.tile_count - 1);
  ivec2 tile_hi = min(px_hi / LIGHT_TILE_SIZE, game_lighting_buf.tile_count - 1);
  int word_count = game_lighting_buf.word_count;
  float log_ratio = log(z_far / z_near);

  /* Sample of this frame: the whole column moves by the NDC jitter, as the
   * projection of the voxelized media, and each froxel toward the camera by
   * a depth noise in [0, 1) slice: EEVEE's eval_froxel and
   * volume_froxel_jitter, so the voxels and the lights are sampled at the
   * same points. */
  vec2 uv_center = (vec2(froxel_xy) + 0.5) / vec2(grid_size.xy);
  vec2 uv = uv_center - 0.5 * jitter.xy;
  float depth_noise = fract(
      jitter.z + 52.9829189 * fract(0.06711056 * float(froxel_xy.x) +
                                    0.00583715 * float(froxel_xy.y)));
  /* d_jittered = d_center * (z_far / z_near)^(-noise / grid_z). */
  float depth_jitter_scale = exp(-log_ratio * depth_noise / float(grid_size.z));

  /* The jittered column can leave the column span by half a froxel: add the
   * tile it samples. */
  ivec2 tile_sample = clamp(ivec2(uv * vec2(extent)) / LIGHT_TILE_SIZE,
                            ivec2(0),
                            game_lighting_buf.tile_count - 1);
  tile_lo = min(tile_lo, tile_sample);
  tile_hi = max(tile_hi, tile_sample);

  /* Centers place the boxes, whose edge coverage is already a filter, and
   * the history lookup; the jittered samples the lights. */
  vec3 origin_center, ray_center, origin, ray;
  froxel_column(uv_center, origin_center, ray_center);
  froxel_column(uv, origin, ray);
  /* Toward the camera, constant along a column. */
  vec3 view_dir = -normalize(ray);

  /* History: depth in the previous view is the dot with its view z row. */
  vec4 history_depth_row = -vec4(history_viewmat[0][2],
                                 history_viewmat[1][2],
                                 history_viewmat[2][2],
                                 history_viewmat[3][2]);
  float history_inv_log_ratio = 1.0 / log(history_z_far / history_z_near);

  /* Camera axes in world space: rows of the view rotation. */
  vec3 cam_right = vec3(viewmat[0][0], viewmat[1][0], viewmat[2][0]);
  vec3 cam_up    = vec3(viewmat[0][1], viewmat[1][1], viewmat[2][1]);
  vec3 cam_back  = vec3(viewmat[0][2], viewmat[1][2], viewmat[2][2]);

  for (int z = 0; z < grid_size.z; z++) {
    ivec3 froxel = ivec3(froxel_xy, z);
    float depth_center = z_near * exp(log_ratio * (float(z) + 0.5) / float(grid_size.z));
    vec4  center_pos4  = vec4(origin_center + ray_center * depth_center, 1.0);
    float view_depth   = depth_center * depth_jitter_scale;
    vec3  ws_pos       = origin + ray * view_depth;

    /* Half extents of the froxel along the camera axes: a 1 / grid fraction
     * of the screen laterally, half a slice in depth. */
    vec3 froxel_half = vec3(depth_center * froxel_half_tan,
                            0.5 * depth_center * log_ratio / float(grid_size.z));

    /* Medium of the froxel: sum of the constant media, each weighted by the
     * fraction of the froxel inside its box, so edges ramp over one froxel
     * instead of stepping at its center. The anisotropy is averaged over the
     * scattering media. */
    vec3  sigma_s  = vec3(0.0);
    vec3  sigma_a  = vec3(0.0);
    vec3  emission = vec3(0.0);
    float g_sum    = 0.0;
    float g_weight = 0.0;
    for (int b = 0; b < volume_box_count; b++) {
      VolumeBoxData box = volume_box_buf[b];
      vec3 unit = vec3(dot(box.world_to_unit_x, center_pos4),
                       dot(box.world_to_unit_y, center_pos4),
                       dot(box.world_to_unit_z, center_pos4));
      /* Froxel half extent along each box axis, in unit box coordinates:
       * support of the camera aligned froxel box, |J_a . e_i| h_i summed. */
      vec3 radius = vec3(
          dot(abs(vec3(dot(box.world_to_unit_x.xyz, cam_right),
                       dot(box.world_to_unit_x.xyz, cam_up),
                       dot(box.world_to_unit_x.xyz, cam_back))),
              froxel_half),
          dot(abs(vec3(dot(box.world_to_unit_y.xyz, cam_right),
                       dot(box.world_to_unit_y.xyz, cam_up),
                       dot(box.world_to_unit_y.xyz, cam_back))),
              froxel_half),
          dot(abs(vec3(dot(box.world_to_unit_z.xyz, cam_right),
                       dot(box.world_to_unit_z.xyz, cam_up),
                       dot(box.world_to_unit_z.xyz, cam_back))),
              froxel_half));
      /* Unbounded medium: zero matrix, zero radius. */
      radius = max(radius, vec3(1e-6));
      /* Overlap of [unit - r, unit + r] with [-1, 1] per axis; the product
       * is exact for an axis aligned froxel, close otherwise. */
      vec3 overlap = min(unit + radius, vec3(1.0)) - max(unit - radius, vec3(-1.0));
      vec3 cover3 = clamp(overlap / (2.0 * radius), 0.0, 1.0);
      float coverage = cover3.x * cover3.y * cover3.z;
      if (coverage <= 0.0) {
        continue;
      }
      sigma_s  += box.scattering.rgb * coverage;
      sigma_a  += box.absorption.rgb * coverage;
      emission += box.emission.rgb * coverage;
      if (any(greaterThan(box.scattering.rgb, vec3(0.0)))) {
        g_sum    += box.scattering.a * coverage;
        g_weight += coverage;
      }
    }
    /* Voxelized media, written by their shaders at this same froxel. The
     * extinction grid holds scattering + absorption. */
    vec3 sigma_e_voxels = vec3(0.0);
    if (use_voxels != 0) {
      vec3 voxel_s    = texelFetch(prop_scattering_tx, froxel, 0).rgb;
      sigma_e_voxels  = texelFetch(prop_extinction_tx, froxel, 0).rgb;
      sigma_s        += voxel_s;
      emission       += texelFetch(prop_emission_tx, froxel, 0).rgb;
      g_sum          += texelFetch(prop_phase_tx, froxel, 0).r;
      g_weight       += texelFetch(prop_phase_weight_tx, froxel, 0).r;
      /* sigma_s now includes voxel_s, which the extinction grid has too. */
      sigma_e_voxels -= voxel_s;
    }
    /* Clamp g to [-0.99, 0.99]: the Henyey-Greenstein denominator
     *   (1 + g^2 - 2g cos(theta))^(3/2)
     * reaches zero at g = +-1, and the Inf would spread through the whole
     * column in the integration. */
    float g = clamp(g_sum / max(g_weight, 1.0), -0.99, 0.99);
    vec3 sigma_t = sigma_s + sigma_a + sigma_e_voxels;

    vec3 in_scatter = emission;

    if (any(greaterThan(sigma_s, vec3(0.0)))) {
      for (int w = 0; w < word_count; w++) {
        uint bits = 0u;
        for (int ty = tile_lo.y; ty <= tile_hi.y; ty++) {
          for (int tx = tile_lo.x; tx <= tile_hi.x; tx++) {
            int tile = ty * game_lighting_buf.tile_count.x + tx;
            bits |= light_tile_mask_buf[tile * word_count + w];
          }
        }
        while (bits != 0u) {
          int bit = findLSB(bits);
          bits &= bits - 1u;
          LightData light = light_buf[w * 32 + bit];

          vec3 L;
          vec3 irradiance = light_incoming(light, ws_pos, L);
          if (all(equal(irradiance, vec3(0.0)))) {
            continue;
          }
          /* theta between the travel direction of the light (-L) and the
           * way to the camera: forward scattering (g > 0) peaks when looking
           * toward the light. */
          float phase = hg_phase(dot(-L, view_dir), g);
          float shadow = (light.shadow_index >= 0) ?
                             game_light_shadow_volume(light, ws_pos, view_depth) :
                             1.0;
          in_scatter += sigma_s * irradiance * phase * shadow;
        }
      }
    }

    /* Exponential moving average with last frame at the same world point:
     * the jitter of the 8 or so frames it holds spreads the samples over
     * the froxel. Off the previous grid, this frame alone. */
    if (history_opacity > 0.0) {
      vec4 hs = history_viewproj * center_pos4;
      float history_depth = dot(history_depth_row, center_pos4);
      if (hs.w > 0.0 && history_depth > 0.0) {
        vec3 uvw_history = vec3(hs.xy / hs.w * 0.5 + 0.5,
                                log(history_depth / history_z_near) * history_inv_log_ratio);
        if (all(greaterThanEqual(uvw_history, vec3(0.0))) &&
            all(lessThanEqual(uvw_history, vec3(1.0))))
        {
          vec3 scatter_history = textureLod(scatter_history_tx, uvw_history, 0.0).rgb;
          vec3 extinction_history = textureLod(extinction_history_tx, uvw_history, 0.0).rgb;
          in_scatter = mix(in_scatter, scatter_history, history_opacity);
          sigma_t = mix(sigma_t, extinction_history, history_opacity);
        }
      }
      /* A NaN or Inf would otherwise live in the history forever and leak
       * to the neighbors through the filtering. */
      if (any(isnan(in_scatter)) || any(isinf(in_scatter)) || any(isnan(sigma_t)) ||
          any(isinf(sigma_t)))
      {
        in_scatter = vec3(0.0);
        sigma_t = vec3(0.0);
      }
    }

    imageStore(out_grid_img, froxel, vec4(in_scatter, 0.0));
    imageStore(out_extinction_img, froxel, vec4(sigma_t, 0.0));
  }
}
