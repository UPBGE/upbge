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

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

/* Froxel scatter/extinction grid.  One entry per froxel.
 * RGB = σ_s × L_in  (light contribution weighted by scattering coefficient)
 * A   = σ_t         (total extinction = scattering + absorption) */
layout(rgba16f) uniform writeonly image3D out_grid_img;

/* Shadow atlas for CSM directional shadow lookup (binding 0). */
uniform sampler2DShadow shadow_atlas;

/* Light SSBO — packed LightData structs (match C++ LightData). */
struct LightData {
  vec3  position;
  uint  type;      /* 0=point, 1=spot, 2=sun */
  vec3  color;
  float energy;
  vec3  direction;
  float radius;
  float attenuation;
  float spot_angle;
  int   shadow_index;
  float _pad0;
};
layout(std430, binding = 0) readonly buffer LightBuf { LightData light_buf[]; };

/* Froxel grid parameters (must match VolumeSettings in C++). */
uniform int   tile_size;    /* Screen-space pixels per froxel XY cell */
uniform int   samples_z;    /* Depth slices in the frustum-aligned grid */
uniform float vol_density;  /* Homogeneous medium extinction σ_t */
uniform float vol_anisotropy; /* Henyey-Greenstein g parameter */
uniform int   light_count;

/* Camera / view data. */
uniform mat4  viewmat;
uniform mat4  viewprojinv;
uniform vec3  camera_pos;
uniform float z_near;
uniform float z_far;
uniform vec2  screen_res;

/* CSM shadow atlas cascade 0 view-projection matrix (directional only). */
uniform mat4  cascade_viewproj;

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

  return texture(shadow_atlas, shadow_uv3);
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

  /* Homogeneous medium: σ_s and σ_a are constant throughout the frustum.
   * σ_t = vol_density, σ_s = σ_t (pure scattering, no absorption for simple fog). */
  const float sigma_t = vol_density;
  const float sigma_s = vol_density * 0.9; /* 90% scattering albedo */

  /* Camera view direction for phase function (V = -z_view in camera space). */
  vec3 V = normalize(camera_pos);  /* Approximation: will be overridden per-froxel */

  for (int z = 0; z < grid_size.z; z++) {
    ivec3 froxel  = ivec3(froxel_xy, z);
    vec3  ws_pos  = froxel_to_world(froxel, vec3(grid_size));

    /* View direction toward camera from this froxel. */
    vec3  view_dir = normalize(camera_pos - ws_pos);

    vec3 in_scatter = vec3(0.0);

    /* Iterate over all lights.  In practice this is bounded by MAX_LIGHTS (typically 64).
     * Only directional (sun) lights contribute here; punctual lights are cheaper to
     * composite in the resolve pass using the shadow mask. */
    for (int i = 0; i < light_count; i++) {
      LightData light = light_buf[i];

      if (light.type == 2u) {
        /* Sun / directional light. */
        vec3  L         = normalize(-light.direction);
        float cos_theta = dot(L, view_dir);
        float phase     = hg_phase(cos_theta, vol_anisotropy);

        /* Shadow visibility: 1.0 = fully lit, 0.0 = in shadow. */
        float shadow = shadow_directional(ws_pos);

        /* Radiance contribution: σ_s × L_i × phase × shadow. */
        in_scatter += sigma_s * light.color * light.energy * phase * shadow;
      }
      /* Punctual lights: handled in volume_resolve by sampling shadow_mask_tx.
       * Adding them here would require per-light shadow atlas tile lookups which
       * are bandwidth-expensive in a 3D dispatch.  The resolve pass is faster
       * because it works in 2D screen space where the shadow mask is already computed. */
    }

    /* Write: RGB = in-scattered radiance, A = extinction. */
    imageStore(out_grid_img, froxel, vec4(in_scatter, sigma_t));
  }
}
