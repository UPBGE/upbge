/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_vector.hh"
#include "BLI_math_matrix.hh"

namespace blender::eevee_game {

/* --- ENGINE CONSTANTS --- */

#define MAX_LIGHTS           512    /* Maximum lights for Tiled Deferred evaluation */
#define MAX_SHADOW_CASCADES  4      /* Standard CSM cascades */
#define MAX_GPU_INSTANCES    65536  /* Maximum objects for GPU-Driven Culling */
#define SHADOW_ATLAS_RES     4096   /* Resolution of the fixed shadow atlas */
#define VOLUME_FROXEL_Z      64     /* Depth slices for volumetric fog */
#define RAY_STEPS_MAX        128    /* Maximum steps for SSR/SSGI tracing */
#define RAYTRACE_GROUP_SIZE  8      /* Compute shader local_size for SSR dispatch */

/* --- RESOURCE BINDING SLOTS --- */

/* Texture Slots - must match GLSL binding declarations */
#define SLOT_GBUFFER_HEADER   0
#define SLOT_GBUFFER_CLOSURE  1
#define SLOT_GBUFFER_NORMAL   2
#define SLOT_SHADOW_ATLAS     3
#define SLOT_HIZ_BUFFER       4
#define SLOT_LTC_LUT          5  /* Area Light lookup table */
#define SLOT_SMAA_AREA_LUT    6
#define SLOT_SMAA_SEARCH_LUT  7
#define SLOT_SSGI_RESULT      8

/* Buffer Slots (UBO/SSBO) - must match GLSL binding declarations */
#define SLOT_UNIFORM_DATA     0  /* Main Scene/View data */
#define SLOT_LIGHT_DATA       1  /* Light source array */
#define SLOT_CULLING_DATA     2  /* Object transforms for GPU Culling */
#define SLOT_SHADOW_DATA      3  /* CSM Matrices and PCSS params */

/* --- ENUMERATIONS ---
 * AAMode and UpscaleMode are defined here only.
 * eevee_game_pipeline.hh must NOT redefine AAMode. */

enum class LightType : uint32_t {
  SUN            = 0,
  PUNCTUAL_POINT = 1,
  PUNCTUAL_SPOT  = 2,
  AREA_RECT      = 3,
  AREA_ELLIPSE   = 4
};

enum class AAMode : uint32_t {
  NONE = 0,
  FXAA = 1,
  SMAA = 2
};

enum class UpscaleMode : uint32_t {
  OFF                = 0,
  FSR2_ULTRA_QUALITY = 1,
  FSR2_QUALITY       = 2,
  FSR2_BALANCED      = 3,
  FSR2_PERFORMANCE   = 4
};

/* Stencil bits for Deferred Hybrid material classification */
enum StencilBits {
  STENCIL_OPAQUE         = (1 << 0),
  STENCIL_TRANSPARENT    = (1 << 1),
  STENCIL_HAIR           = (1 << 2),
  STENCIL_REFRACTIVE     = (1 << 3),
  STENCIL_RECEIVE_SHADOW = (1 << 4),
};

/* --- GPU STRUCTURES (std140 Aligned) ---
 * Every float3 is followed by a scalar to satisfy std140 vec4 alignment.
 * Violating this causes misaligned reads in the shader with no compiler warning. */

struct UniformData {
  float4x4 viewmat;
  float4x4 projectionmat;
  float4x4 viewprojmat;
  float4x4 viewinv;

  float3   camera_pos;
  float    time;

  float2   screen_res;
  float2   screen_res_inv;

  float    z_near;
  float    z_far;
  float    delta_time;
  uint32_t frame_count;

  /* Halton(2,3) sub-pixel jitter for FSR temporal reconstruction */
  float2   jitter;
  uint32_t aa_mode;
  float    exposure;
};

struct LightData {
  float3   position;
  uint32_t type;

  float3   color;
  float    energy;

  float3   direction;    /* For Spot and Sun lights */
  float    radius;       /* Light size for PCSS/Area lights */

  float    attenuation;
  float    spot_angle;
  int32_t  shadow_index; /* Index in Shadow Atlas (-1 = no shadow) */
  float    _pad0;
};

struct ShadowUniformData {
  float4x4 cascade_viewproj[MAX_SHADOW_CASCADES];
  float4   cascade_splits;

  float    pcss_light_radius;
  float    pcss_filter_min;
  float    shadow_bias;
  uint32_t shadow_map_res;
};

/* GPU instance data for culling and indirect draw.
 * Layout (std430 compatible, matches GLSL struct in eevee_game_culling_comp.glsl):
 *   float4x4 model_matrix  = 64 bytes
 *   float3 bb_min + uint   = 16 bytes  (interleaved to avoid padding)
 *   float3 bb_max + uint   = 16 bytes
 *   Total: 96 bytes, 16-byte aligned.
 *
 * The AABB is stored in local space; the culling shader transforms via model_matrix. */
struct alignas(16) GPUInstanceData {
  float4x4 model_matrix;
  float3   bb_min;       /* AABB min in local space */
  uint32_t resource_id;  /* DRW resource handle, indexes the per-object UBO */
  float3   bb_max;       /* AABB max in local space */
  uint32_t flags;        /* Bit flags: shadow caster, transparent, etc. */
};
static_assert(sizeof(GPUInstanceData) == 96,
              "GPUInstanceData size must match GLSL std430 layout");

/* --- CLOSURE DEFINITIONS ---
 * Bitmask for active closures in a material.
 * Used by the Tiled Deferred loop to skip unnecessary evaluations. */
enum ClosureBits {
  CLOSURE_NONE        = 0,
  CLOSURE_DIFFUSE     = (1 << 0),
  CLOSURE_GLOSSY      = (1 << 1),
  CLOSURE_REFRACTION  = (1 << 2),
  CLOSURE_TRANSLUCENT = (1 << 3),
  CLOSURE_SSS         = (1 << 4),
  CLOSURE_EMISSION    = (1 << 5),
};

/* Settings for the FSR upscaling module, stored on GameInstance */
struct UpscaleSettings {
  UpscaleMode mode = UpscaleMode::OFF;
};

} // namespace blender::eevee_game
