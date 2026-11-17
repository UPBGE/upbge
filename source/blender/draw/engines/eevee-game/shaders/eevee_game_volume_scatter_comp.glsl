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
 * Maps (froxel_xy, depth_slice) to a world-space position.
 * Exponential Z spacing: slice index i maps to view-depth z as:
 *   z(i) = z_near * (z_far / z_near)^(i / samples_z)
 * This matches the integration pass which uses the same spacing. */
vec3 froxel_to_world(ivec3 froxel, vec3 grid_size)
{
  /* Normalised froxel centre in [0,1]^3 */
  vec3 uvw = (vec3(froxel) + 0.5) / grid_size;

  /* Screen-space XY centre. */
  vec2 screen_uv = uvw.xy;

  /* Exponential Z: remap uvw.z from [0,1] to view-depth [z_near, z_far]. */
  float view_depth = z_near * pow(z_far / z_near, uvw.z);

  /* NDC Z corresponding to this view depth.
   * OpenGL: z_ndc = ((z_far + z_near) - 2*z_near*z_far/view_depth) / (z_far - z_near) */
  float z_ndc = ((z_far + z_near) - 2.0 * z_near * z_far / view_depth) / (z_far - z_near);

  /* Reconstruct world position. */
  vec4 ndc = vec4(screen_uv * 2.0 - 1.0, z_ndc, 1.0);
  vec4 ws  = viewprojinv * ndc;
  return ws.xyz / ws.w;
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

  for (int z = 0; z < grid_size.z; z++) {
    ivec3 froxel  = ivec3(froxel_xy, z);
    vec3  ws_pos  = froxel_to_world(froxel, vec3(grid_size));
    vec4  ws_pos4 = vec4(ws_pos, 1.0);

    /* Medium at the froxel center: sum of the constant media whose box holds
     * it. The anisotropy is averaged over the scattering media. */
    vec3  sigma_s  = vec3(0.0);
    vec3  sigma_a  = vec3(0.0);
    vec3  emission = vec3(0.0);
    float g_sum    = 0.0;
    float g_weight = 0.0;
    for (int b = 0; b < volume_box_count; b++) {
      VolumeBoxData box = volume_box_buf[b];
      vec3 unit = vec3(dot(box.world_to_unit_x, ws_pos4),
                       dot(box.world_to_unit_y, ws_pos4),
                       dot(box.world_to_unit_z, ws_pos4));
      if (any(greaterThan(abs(unit), vec3(1.0)))) {
        continue;
      }
      sigma_s  += box.scattering.rgb;
      sigma_a  += box.absorption.rgb;
      emission += box.emission.rgb;
      if (any(greaterThan(box.scattering.rgb, vec3(0.0)))) {
        g_sum    += box.scattering.a;
        g_weight += 1.0;
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

    /* View direction toward camera from this froxel. */
    vec3  view_dir = normalize(camera_pos - ws_pos);

    vec3 in_scatter = emission;

    if (any(greaterThan(sigma_s, vec3(0.0)))) {
      /* Same slicing as froxel_to_world(): picks the sun cascade. */
      float view_depth = z_near * exp(log_ratio * (float(z) + 0.5) / float(grid_size.z));
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

    imageStore(out_grid_img, froxel, vec4(in_scatter, 0.0));
    imageStore(out_extinction_img, froxel, vec4(sigma_t, 0.0));
  }
}
