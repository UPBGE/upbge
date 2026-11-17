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

/* out_grid_img, one entry per froxel:
 *   RGB = sigma_s * L_in  (light contribution weighted by scattering coefficient)
 *   A   = sigma_t         (total extinction = scattering + absorption)
 *
 * light_buf[] is LightData from eevee_game_shader_shared.hh.
 * cascade_viewproj is cascade 0 of the sun, the only shadow sampled here. */

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

/* ------------------------------------------------------------------ */
/* Cascade 0 shadow lookup for directional lights.
 * Returns 1.0 (lit) or in [0,1] (penumbra via hardware PCF). */
float shadow_directional(vec3 world_pos)
{
  vec4 light_clip  = cascade_viewproj * vec4(world_pos, 1.0);
  vec3 light_ndc   = light_clip.xyz / light_clip.w;
  vec3 shadow_uv3  = light_ndc * 0.5 + 0.5;

  if (any(lessThan(shadow_uv3.xy, vec2(0.0))) ||
      any(greaterThan(shadow_uv3.xy, vec2(1.0))))
  {
    return 1.0; /* Outside cascade: assume lit */
  }

  /* Cascade 0 is the first 1024 px tile of the 4096 px static atlas: the
   * bottom-left quarter in each axis. */
  vec2 atlas_uv = shadow_uv3.xy * 0.25;
  /* The atlas stores reversed depth (1 = near the light), so the reference
   * is reversed too. The compare is `ref <= stored` (LEQUAL): true when a
   * surface at least as near to the light is there, the occluded case. */
  return 1.0 - texture(shadow_atlas, vec3(atlas_uv, 1.0 - shadow_uv3.z));
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
    /* One extinction for the three channels: the grid has one alpha. */
    float sigma_t = dot(sigma_s + sigma_a + sigma_e_voxels, vec3(1.0 / 3.0));

    /* View direction toward camera from this froxel. */
    vec3  view_dir = normalize(camera_pos - ws_pos);

    vec3 in_scatter = emission;

    if (any(greaterThan(sigma_s, vec3(0.0)))) {
      for (int i = 0; i < light_count; i++) {
        LightData light = light_buf[i];

        if (light.type == LIGHT_TYPE_SUN) {
          /* theta between the travel direction of the light and the way to
           * the camera: forward scattering (g > 0) peaks when looking toward
           * the sun. */
          float cos_theta = dot(light.direction, view_dir);
          float phase     = hg_phase(cos_theta, g);

          /* Shadow visibility: 1.0 = fully lit, 0.0 = in shadow. */
          float shadow = shadow_directional(ws_pos);

          in_scatter += sigma_s * light.color * light.energy * phase * shadow;
        }
        /* Punctual lights do not reach the volume yet. */
      }
    }

    /* Write: RGB = in-scattered radiance, A = extinction. */
    imageStore(out_grid_img, froxel, vec4(in_scatter, sigma_t));
  }
}
