/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Structures shared between C++ and GLSL.
 *
 * Anything a shader reads from a buffer has to be declared here and nowhere
 * else, so both sides compile the same layout. Only add data that actually
 * crosses to the GPU; CPU-only bookkeeping stays in the module headers.
 */

#pragma once

#include "GPU_shader_shared_utils.hh"

#ifndef GPU_SHADER
namespace blender::eevee_game {
#endif

/* GameLightData::type. Mirrors `enum class LightType` (eevee_game_defines.hh),
 * which static-asserts against these so the two cannot drift apart. */
#define LIGHT_TYPE_SUN 0u
#define LIGHT_TYPE_POINT 1u
#define LIGHT_TYPE_SPOT 2u
#define LIGHT_TYPE_AREA_RECT 3u
#define LIGHT_TYPE_AREA_ELLIPSE 4u

/* One light, 64 bytes: exactly one cache line per light in the shading loop.
 * Each float3 is followed by a scalar so the std430 layout has no implicit
 * padding and matches the C++ struct byte for byte. */
struct [[host_shared]] GameLightData {
  packed_float3 position;
  uint type;

  /** Color times power: radiant intensity (W/sr) of point, spot and area
   * lights (the latter on their axis), irradiance (W/m2) of suns. */
  packed_float3 color;
  /** Diffuse (x) and specular (y) factors of the light, half floats. */
  uint diffuse_specular;

  /** Spot, area and sun lights. */
  packed_float3 direction;
  /** Emitter radius: sphere radius, half the largest side of an area light,
   * sine of the angular radius of a sun. */
  float radius;

  /** Influence distance; the light contributes nothing beyond it. */
  float attenuation;
  /** Spot cone, half floats: 1 - cos(half angle) (x), and the width of the
   * blend band in the same unit (y). Small values stay small, so the halves
   * keep their precision on narrow cones (an inverse width would overflow). */
  uint spot_shape;
  /** Shadow atlas slot, -1 = no shadow. */
  int shadow_index;
  /** Factor of the light in volume scattering. */
  float volume_factor;
};

/* Fraction of a sun cascade's split depth over which it fades into the next
 * cascade (the last one into no shadow). Each cascade is fitted to also cover
 * the band of the previous one. */
#define SHADOW_CASCADE_BLEND 0.1f

/* One rendered shadow face: a sun cascade (four consecutive entries), a spot
 * light, or one cube face of a point light (six consecutive entries ordered
 * +X -X +Y -Y +Z -Z). GameLightData::shadow_index is the index of the first. */
struct [[host_shared]] GameShadowTileData {
  float4x4 view_proj;
  /** Atlas UV rectangle of the face: min in xy, size in zw. */
  float4 atlas_rect;
  /** 0: static atlas, 1: dynamic atlas. */
  int atlas;
  /** World size of one texel: at unit distance from the light for a
   * perspective face (2 tan(half fov) / resolution), absolute for a cascade. */
  float texel_scale;
  /** Cascades: view depth up to which this cascade is used. */
  float split_depth;
  float _pad0;
};

/* Screen tiles of the light culling, in pixels of the render extent. Equal
 * to Hi-Z mip 4 texels: the farthest depth of a tile is one fetch. */
#define LIGHT_TILE_SIZE 16
#define LIGHT_TILE_HIZ_LOD 4

/* Per frame lighting constants, shared by the light culling, the deferred
 * lighting and the forward surfaces. */
struct [[host_shared]] GameLightingData {
  int light_count;
  /** 32 lights per word of the tile masks: ceil(light_count / 32). */
  int word_count;
  int2 tile_count;
  int2 render_extent;
  /** Fog of the forward surfaces: froxel slice w = log(z / near) * log_inv. */
  bool32_t use_volume;
  float volume_z_near;
  float volume_log_inv;
  /** Screen-space reflections of the forward surfaces (see RayTraceModule). */
  bool32_t use_ssr;
  int ssr_max_steps;
  float ssr_max_roughness;
  float ssr_thickness;
  /** Camera far clip distance, with volume_z_near the depth linearization of the trace. */
  float z_far;
  int _pad0;
  int _pad1;
};

/* Participating medium that is constant inside a box, evaluated
 * analytically by the volume scatter pass instead of being voxelized. A zero
 * matrix maps every point to the box center: an unbounded medium (world). */
struct [[host_shared]] VolumeBoxData {
  /** Rows of the affine world to unit box [-1, 1]^3 transform. */
  float4 world_to_unit_x;
  float4 world_to_unit_y;
  float4 world_to_unit_z;
  /** Scattering coefficient (RGB) and Henyey-Greenstein anisotropy (A). */
  float4 scattering;
  /** Absorption coefficient (RGB). */
  float4 absorption;
  /** Emitted radiance per unit length (RGB). */
  float4 emission;
};
#define VOLUME_BOX_MAX 64

/* World lighting cube map: mip m is the radiance pre-filtered for the GGX
 * perceptual roughness m / WORLD_CUBE_LOD_MAX. */
#define WORLD_CUBE_MIP_COUNT 6
#define WORLD_CUBE_LOD_MAX 5.0f
/* Face size of the mip the diffuse spherical harmonics are projected from. */
#define WORLD_SH_RES 16

#ifndef GPU_SHADER
}  // namespace blender::eevee_game
#endif
