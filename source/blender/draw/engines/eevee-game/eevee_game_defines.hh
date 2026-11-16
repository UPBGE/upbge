/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"

/* Draw manager vocabulary used by every module: Texture, TextureFromPool,
 * Framebuffer, PassSimple, PassMain, View, Manager. */
#include "DRW_gpu_wrapper.hh"
#include "DRW_render.hh"
#include "draw_manager.hh"
#include "draw_pass.hh"
#include "draw_view.hh"

#include "eevee_game_shader_shared.hh"

namespace blender::eevee_game {

using namespace draw;

/* Size of a raw GPU texture handle. draw::Texture has size(), a bare
 * gpu::Texture * coming through a function argument does not. */
inline int2 texture_extent(gpu::Texture *tx)
{
  return int2(GPU_texture_width(tx), GPU_texture_height(tx));
}

/* --- ENGINE CONSTANTS --- */

#define MAX_LIGHTS           512    /* Maximum lights for Tiled Deferred evaluation */
#define MAX_SHADOW_CASCADES  4      /* Standard CSM cascades */
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
  FSR3_ULTRA_QUALITY = 1,
  FSR3_QUALITY       = 2,
  FSR3_BALANCED      = 3,
  FSR3_PERFORMANCE   = 4
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

/* LightData lives in eevee_game_shader_shared.hh: shaders read it too. */
static_assert(sizeof(LightData) == 64, "LightData must stay one cache line / std430 clean");
static_assert(uint32_t(LightType::SUN) == LIGHT_TYPE_SUN &&
                  uint32_t(LightType::PUNCTUAL_POINT) == LIGHT_TYPE_POINT &&
                  uint32_t(LightType::PUNCTUAL_SPOT) == LIGHT_TYPE_SPOT &&
                  uint32_t(LightType::AREA_RECT) == LIGHT_TYPE_AREA_RECT &&
                  uint32_t(LightType::AREA_ELLIPSE) == LIGHT_TYPE_AREA_ELLIPSE,
              "LightType and the LIGHT_TYPE_* shader defines must match");

struct ShadowUniformData {
  float4x4 cascade_viewproj[MAX_SHADOW_CASCADES];
  float4   cascade_splits;

  float    pcss_light_radius;
  float    pcss_filter_min;
  float    shadow_bias;
  uint32_t shadow_map_res;
};

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
