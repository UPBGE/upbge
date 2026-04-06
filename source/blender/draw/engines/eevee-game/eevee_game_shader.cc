/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_shader.hh"

#include "GPU_material.hh"
#include "DNA_material_types.h"

/* Delegate material shaders to EEVEE's ShaderModule singleton.
 * eevee::ShaderModule owns codegen_callback and pass_replacement_cb closures
 * required for correct material shader compilation. GPUMaterials are cached
 * per-nodetree in blender_mat->gpumaterial with key GPU_MAT_EEVEE. */
#include "../eevee/eevee_shader.hh"
#include "../eevee/eevee_material.hh"

namespace blender::eevee_game {

ShaderModule::ShaderModule()
{
  for (int i = 0; i < MAX_SHADER_TYPE; i++) {
    shaders_[i].name   = static_shader_name_get(static_cast<eShaderType>(i));
    shaders_[i].shader = nullptr;
  }
}

ShaderModule::~ShaderModule()
{
  for (int i = 0; i < MAX_SHADER_TYPE; i++) {
    if (shaders_[i].shader) {
      GPU_shader_free(shaders_[i].shader);
      shaders_[i].shader = nullptr;
    }
  }
}

/* static */
void ShaderModule::module_free()
{
  /* Instance destructor frees per-shader objects.
   * This hook exists for Engine::free_static() at Blender shutdown. */
}

gpu::Shader *ShaderModule::static_shader_get(eShaderType type)
{
  BLI_assert(type >= 0 && type < MAX_SHADER_TYPE);
  if (shaders_[type].shader == nullptr) {
    /* Synchronous fallback compile — only hit if static_shaders_load() was not
     * called at init time.  This stalls the render thread for the duration of
     * shader compilation, which can be 50–200ms per shader on cold start.
     * Call static_shaders_load(SG_ALL) from GameInstance::init() to avoid this. */
    shaders_[type].shader = GPU_shader_create_from_info_name(shaders_[type].name);
    BLI_assert_msg(shaders_[type].shader != nullptr,
                   "eevee_game: shader compilation failed — check ShaderCreateInfo name");
  }
  return shaders_[type].shader;
}

/* static */
const char *ShaderModule::static_shader_name_get(eShaderType type)
{
  switch (type) {
    /* --- Deferred Pipeline --- */
    case SH_DEFERRED_LIGHT:          return "eevee_game_deferred_light";
    case SH_DEFERRED_COMBINE:        return "eevee_game_deferred_combine";
    case SH_DEFERRED_TILE_CLASSIFY:  return "eevee_game_tile_classify";
    case SH_GBUFFER:                 return "eevee_game_gbuffer";

    /* --- Shadows --- */
    case SH_SHADOW_DIRECTIONAL_CSM:  return "eevee_game_shadow_csm";
    case SH_SHADOW_PUNCTUAL_ATLAS:   return "eevee_game_shadow_atlas";
    case SH_SHADOW_PCSS_FILTER:      return "eevee_game_shadow_pcss";

    /* --- SSR --- */
    case SH_SSR_TRACE:               return "eevee_game_ssr_hiz_trace";

    /* --- GTAO --- */
    case SH_GTAO_MAIN:               return "eevee_game_gtao_main";
    case SH_GTAO_UPSAMPLE:           return "eevee_game_gtao_upsample";

    /* --- SSGI --- */
    case SH_SSGI_MAIN:               return "eevee_game_ssgi_main";
    case SH_SSGI_BLUR:               return "eevee_game_ssgi_blur";

    /* --- Bloom ---
     * FIX: SH_BLOOM_COMPOSITE was missing from this switch; it would hit the
     * default: branch and call BLI_assert_unreachable(), crashing on first sync(). */
    case SH_BLOOM_DOWNSAMPLE:        return "eevee_game_bloom_downsample";
    case SH_BLOOM_UPSAMPLE:          return "eevee_game_bloom_upsample";
    case SH_BLOOM_COMPOSITE:         return "eevee_game_bloom_composite";

    /* --- Volumetrics ---
     * FIX: SH_VOLUME_RESOLVE was missing. VolumeModule::resolve() calls
     * static_shader_get(SH_VOLUME_RESOLVE) but the original switch had no case for it,
     * causing the resolve pass to use the integration shader — wrong semantics. */
    case SH_VOLUME_SCATTER:          return "eevee_game_volume_scatter";
    case SH_VOLUME_INTEGRATE:        return "eevee_game_volume_integrate";
    case SH_VOLUME_RESOLVE:          return "eevee_game_volume_resolve";

    /* --- Culling --- */
    case SH_CULLING_COMPUTE:         return "eevee_game_culling_compute";

    /* --- Forward --- */
    case SH_FORWARD:                 return "eevee_game_forward";

    /* --- Anti-Aliasing --- */
    case SH_FXAA:                    return "eevee_game_fxaa";
    case SH_SMAA_EDGE:               return "eevee_game_smaa_edge";
    case SH_SMAA_WEIGHT:             return "eevee_game_smaa_weight";
    case SH_SMAA_BLEND:              return "eevee_game_smaa_blend";

    /* --- DoF --- */
    case SH_DOF_COC_SETUP:           return "eevee_game_dof_coc_setup";
    case SH_DOF_BOKEH_BLUR:          return "eevee_game_dof_bokeh_blur";
    case SH_DOF_RESOLVE:             return "eevee_game_dof_resolve";
    case SH_MOTION_BLUR_FAST:        return "eevee_game_motion_blur_fast";

    /* --- Infrastructure --- */
    case SH_HIZ_UPDATE:              return "eevee_game_hiz_update";
    case SH_FILM_PRESENT:            return "eevee_game_film_present";

    default:
      BLI_assert_unreachable();
      return "";
  }
}

void ShaderModule::static_shaders_load(ShaderGroups groups)
{
  /* Compile all shaders in the requested groups synchronously.
   *
   * This is called from GameInstance::init() to front-load all compilation
   * cost before the first rendered frame, eliminating mid-game stutter caused
   * by on-demand synchronous compile in static_shader_get().
   *
   * When Blender exposes a public async shader batch API (e.g. a future
   * GPU_shader_batch_precompile()), replace this loop with non-blocking calls
   * and a completion poll in begin_sync(). For now, synchronous is correct and
   * the only safe option without that API.
   *
   * Group membership is defined here; add new shaders to the appropriate group
   * when they are added to eShaderType. */

  /* Map each shader enum to its group bit. */
  struct ShaderGroupEntry {
    eShaderType type;
    ShaderGroups group;
  };

  static const ShaderGroupEntry entries[] = {
    /* Lighting */
    { SH_DEFERRED_LIGHT,         SG_LIGHTING },
    { SH_DEFERRED_COMBINE,       SG_LIGHTING },
    { SH_DEFERRED_TILE_CLASSIFY, SG_LIGHTING },
    { SH_GBUFFER,                SG_LIGHTING },
    { SH_FORWARD,                SG_LIGHTING },

    /* Shadow */
    { SH_SHADOW_DIRECTIONAL_CSM, SG_SHADOW },
    { SH_SHADOW_PUNCTUAL_ATLAS,  SG_SHADOW },
    { SH_SHADOW_PCSS_FILTER,     SG_SHADOW },

    /* Post FX */
    { SH_BLOOM_DOWNSAMPLE,       SG_POST_FX },
    { SH_BLOOM_UPSAMPLE,         SG_POST_FX },
    { SH_BLOOM_COMPOSITE,        SG_POST_FX },
    { SH_DOF_COC_SETUP,          SG_POST_FX },
    { SH_DOF_BOKEH_BLUR,         SG_POST_FX },
    { SH_DOF_RESOLVE,            SG_POST_FX },
    { SH_MOTION_BLUR_FAST,       SG_POST_FX },
    { SH_VOLUME_SCATTER,         SG_POST_FX },
    { SH_VOLUME_INTEGRATE,       SG_POST_FX },
    { SH_VOLUME_RESOLVE,         SG_POST_FX },
    { SH_FILM_PRESENT,           SG_POST_FX },

    /* AA */
    { SH_FXAA,                   SG_AA },
    { SH_SMAA_EDGE,              SG_AA },
    { SH_SMAA_WEIGHT,            SG_AA },
    { SH_SMAA_BLEND,             SG_AA },

    /* Reflections */
    { SH_SSR_TRACE,              SG_REFLECT },
    { SH_GTAO_MAIN,              SG_REFLECT },
    { SH_GTAO_UPSAMPLE,          SG_REFLECT },
    { SH_SSGI_MAIN,              SG_REFLECT },
    { SH_SSGI_BLUR,              SG_REFLECT },

    /* Infrastructure (always compile) */
    { SH_HIZ_UPDATE,             SG_LIGHTING },
    { SH_CULLING_COMPUTE,        SG_LIGHTING },
  };

  for (const ShaderGroupEntry &e : entries) {
    if (!(groups & e.group)) {
      continue;
    }
    if (shaders_[e.type].shader == nullptr) {
      shaders_[e.type].shader = GPU_shader_create_from_info_name(shaders_[e.type].name);
      BLI_assert_msg(shaders_[e.type].shader != nullptr,
                     "eevee_game: static_shaders_load failed for a shader");
    }
  }
}

GPUMaterial *ShaderModule::material_shader_get(blender::Material *material,
                                                bNodeTree *ntree,
                                                eevee::eMaterialPipeline pipeline_type,
                                                eevee::eMaterialGeometry geometry_type,
                                                bool deferred,
                                                blender::Material *default_mat)
{
  return eevee::ShaderModule::module_get()->material_shader_get(
      material, ntree, pipeline_type, geometry_type, deferred, default_mat);
}

} // namespace blender::eevee_game
