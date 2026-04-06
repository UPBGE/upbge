/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "GPU_shader.hh"
#include "GPU_material.hh"
#include "eevee_game_defines.hh"

/* eMaterialPipeline, eMaterialGeometry — reuse EEVEE's enums which encode
 * the exact shader permutation key used by ShaderModule::material_shader_get(). */
#include "../eevee/eevee_material_shared.hh"

struct bNodeTree;

namespace blender::eevee_game {

/* All static shaders used by eevee_game.
 * Every entry referenced from any .cc must be listed here.
 * Order matters only for readability; the enum values index shaders_[]. */
enum eShaderType {
  /* --- Deferred / Lighting --- */
  SH_DEFERRED_LIGHT = 0,
  SH_DEFERRED_COMBINE,
  SH_DEFERRED_TILE_CLASSIFY,
  SH_GBUFFER,

  /* --- Shadows (Optimized Fixed Atlas) --- */
  SH_SHADOW_DIRECTIONAL_CSM,
  SH_SHADOW_PUNCTUAL_ATLAS,
  SH_SHADOW_PCSS_FILTER,

  /* --- Reflections (Fast Hi-Z SSR) --- */
  SH_SSR_TRACE,

  /* --- Ambient Occlusion (GTAO) --- */
  SH_GTAO_MAIN,
  SH_GTAO_UPSAMPLE,

  /* --- Screen Space Global Illumination --- */
  SH_SSGI_MAIN,
  SH_SSGI_BLUR,

  /* --- Bloom ---
   * FIX: added SH_BLOOM_COMPOSITE so BloomModule::sync() can call
   * static_shader_get(SH_BLOOM_COMPOSITE) without hitting an undefined enum value. */
  SH_BLOOM_DOWNSAMPLE,
  SH_BLOOM_UPSAMPLE,
  SH_BLOOM_COMPOSITE,

  /* --- Volumetrics --- */
  SH_VOLUME_SCATTER,
  SH_VOLUME_INTEGRATE,
  SH_VOLUME_RESOLVE,

  /* --- GPU-Driven Culling --- */
  SH_CULLING_COMPUTE,

  /* --- Forward --- */
  SH_FORWARD,

  /* --- Anti-Aliasing --- */
  SH_FXAA,
  SH_SMAA_EDGE,
  SH_SMAA_WEIGHT,
  SH_SMAA_BLEND,

  /* --- Post Processing --- */
  SH_DOF_COC_SETUP,
  SH_DOF_BOKEH_BLUR,
  SH_DOF_RESOLVE,
  SH_MOTION_BLUR_FAST,

  /* --- Infrastructure --- */
  SH_HIZ_UPDATE,
  SH_FILM_PRESENT,

  MAX_SHADER_TYPE,
};

/* Groups of shaders that can be compiled together at init time to avoid frame stalls. */
enum ShaderGroups {
  SG_NONE     = 0,
  SG_LIGHTING = (1 << 0),
  SG_SHADOW   = (1 << 1),
  SG_POST_FX  = (1 << 2),
  SG_AA       = (1 << 3),
  SG_REFLECT  = (1 << 4),
  SG_ALL      = 0xFF,
};

ENUM_OPERATORS(ShaderGroups, int)

class ShaderModule {
 public:
  ShaderModule();
  ~ShaderModule();

  /* Returns a compiled GPU shader; performs synchronous compile on first call if
   * static_shaders_load() was not called at init time. */
  gpu::Shader *static_shader_get(eShaderType type);

  /**
   * Compile all static shaders in the given group synchronously.
   * Call from GameInstance::init() to avoid per-frame stalls.
   * When Blender's async shader compilation API is available this should
   * be replaced with GPU_shader_batch_precompile().
   */
  void static_shaders_load(ShaderGroups groups);

  /**
   * Compile (or retrieve from cache) a GPUMaterial for the given node tree.
   * Delegates to eevee::ShaderModule to reuse the codegen_callback and
   * pass_replacement_cb without duplicating 1000+ lines of shader create info.
   */
  GPUMaterial *material_shader_get(blender::Material *material,
                                   bNodeTree *ntree,
                                   eevee::eMaterialPipeline pipeline_type,
                                   eevee::eMaterialGeometry geometry_type,
                                   bool deferred,
                                   blender::Material *default_mat);

  /* Maps enum to the ShaderCreateInfo name registered in eevee_game_shader_info.cc */
  static const char *static_shader_name_get(eShaderType type);

  /* Frees shared resources; called from Engine::free_static() at Blender shutdown. */
  static void module_free();

 private:
  struct StaticShader {
    gpu::Shader *shader = nullptr;
    const char  *name   = nullptr;
  };

  StaticShader shaders_[MAX_SHADER_TYPE];
};

} // namespace blender::eevee_game
