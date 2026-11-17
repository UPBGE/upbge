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
namespace blender {
struct World;
}

namespace blender::eevee_game {

/* All static shaders used by eevee_game.
 * Every entry referenced from any .cc must be listed here.
 * Order matters only for readability; the enum values index shaders_[]. */
enum eShaderType {
  /* --- Deferred lighting ---
   * Decodes EEVEE's G-Buffer and lights it with the game light list. */
  SH_DEFERRED_LIGHT = 0,
  /* Per screen tile bitmask of the lights that can reach the tile. */
  SH_LIGHT_CULL,

  /* Deterministic 3x3 PCF shadow mask: compute shader that reads depth + normal
   * from the G-Buffer and writes one shadow factor per pixel into shadow_mask_tx.
   * Runs after the shadow atlas is rendered, before the deferred lighting pass. */
  SH_SHADOW_PCF_FILTER,

  /* --- Reflections (Hi-Z SSR) ---
   * Not implemented yet: static_shader_get() returns null. */
  SH_SSR_TRACE,

  /* --- Ambient Occlusion (GTAO) --- */
  SH_GTAO_MAIN,
  SH_GTAO_UPSAMPLE,

  /* --- Screen Space Global Illumination --- */
  SH_SSGI_MAIN,
  SH_SSGI_BLUR,

  /* --- Bloom --- */
  SH_BLOOM_DOWNSAMPLE,
  SH_BLOOM_UPSAMPLE,
  SH_BLOOM_COMPOSITE,

  /* --- Volumetrics --- */
  SH_VOLUME_SCATTER,
  SH_VOLUME_INTEGRATE,
  SH_VOLUME_RESOLVE,

  /* --- Frustum + Hi-Z occlusion culling --- */
  SH_CULLING_COMPUTE,

  /* --- Anti-Aliasing --- */
  SH_FXAA,
  SH_SMAA_EDGE,
  SH_SMAA_WEIGHT,
  SH_SMAA_BLEND,

  /* --- Post Processing --- */
  SH_DOF_COC_SETUP,
  SH_DOF_BOKEH_BLUR,
  SH_DOF_RESOLVE,

  /* --- Infrastructure --- */
  SH_HIZ_UPDATE,
  SH_FILM_PRESENT,
  /* Camera motion of the pixels without object motion (read by FSR). */
  SH_VELOCITY_RESOLVE,

  /* EEVEE's sort of the volume hit lists into occupancy bits. */
  SH_VOLUME_OCCUPANCY_CONVERT,

  /* --- World lighting (run when the world changes) --- */
  SH_WORLD_SH,
  SH_WORLD_PREFILTER,

  MAX_SHADER_TYPE,
};

/* Geometry shaders (depth prepass, G-Buffer, forward, shadow casters) are not
 * listed: they are GPUMaterial shaders generated per material by EEVEE's
 * codegen, fetched with material_shader_get(). */

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

ENUM_OPERATORS(ShaderGroups)

class ShaderModule {
 public:
  ShaderModule();
  ~ShaderModule();

  /* Returns a compiled GPU shader; performs synchronous compile on first call if
   * static_shaders_load() was not called at init time.
   * Returns null for shader types that have no create info yet; callers of
   * those types must skip their pass. */
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

  /* World radiance shader of the lighting capture (`eevee_game_surf_world`). */
  GPUMaterial *world_shader_get(blender::World *world, bNodeTree *ntree, bool deferred);

  /* Maps enum to the ShaderCreateInfo name (shaders/infos/eevee_game_infos.hh),
   * or null when the shader is not implemented yet. */
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
