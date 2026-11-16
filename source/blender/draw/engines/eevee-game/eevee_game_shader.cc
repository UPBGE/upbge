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

#include "gpu_shader_create_info.hh"

namespace blender::eevee_game {

/* Tags the forward materials of eevee_game. EEVEE packs its material type in
 * bits 0..10 of the uuid and decodes them with masks, so bit 16 is ignored by
 * its codegen. It has to stay below bit 32: the GPU pass cache hashes the
 * uuid as a 32 bit seed, and the game pass must never be mistaken for the
 * EEVEE pass of the same node tree (they differ only by the create info). */
static constexpr uint64_t GAME_FORWARD_UUID_BIT = uint64_t(1) << 16;

/* Runs EEVEE's codegen, then swaps EEVEE's forward surface interface for the
 * game one: same node tree code, same closure setup, lit by the game lights
 * instead of EEVEE's light culling, virtual shadow maps and probes. Can run
 * on a compilation thread: touches nothing but the create info. */
static void game_forward_codegen(void *thunk, GPUMaterial *mat, GPUCodegenOutput *codegen)
{
  static_cast<eevee::ShaderModule *>(thunk)->material_create_info_amend(mat, codegen);

  gpu::shader::ShaderCreateInfo &info = *reinterpret_cast<gpu::shader::ShaderCreateInfo *>(
      codegen->create_info);
  for (gpu::shader::ShaderCreateInfo::AdditionalInfo &additional : info.additional_infos_) {
    if (additional.name == "eevee_surf_forward") {
      additional.name = "eevee_game_surf_forward";
    }
  }
  info.name_ += "_game";
}

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
  if (shaders_[type].name == nullptr) {
    return nullptr;
  }
  if (shaders_[type].shader == nullptr) {
    /* Synchronous fallback compile, only hit if static_shaders_load() was not
     * called at init time. It stalls the render thread for the whole
     * compilation. */
    shaders_[type].shader = GPU_shader_create_from_info_name(shaders_[type].name);
    BLI_assert_msg(shaders_[type].shader != nullptr,
                   "eevee_game: shader compilation failed, check the create info name");
  }
  return shaders_[type].shader;
}

/* static */
const char *ShaderModule::static_shader_name_get(eShaderType type)
{
  switch (type) {
    case SH_DEFERRED_LIGHT:
      return "eevee_game_deferred_light";

    /* No create info and no GLSL yet. The old table aliased it to EEVEE's
     * `eevee_ray_trace_screen`, whose resource interface (ray tile lists,
     * closure bins, sampling data) cannot be satisfied by what this engine
     * binds. */
    case SH_SSR_TRACE:
      return nullptr;

    case SH_SHADOW_PCF_FILTER:
      return "eevee_game_shadow_pcf_compute";

    case SH_GTAO_MAIN:
      return "eevee_game_gtao_main";
    case SH_GTAO_UPSAMPLE:
      return "eevee_game_gtao_upsample";

    case SH_SSGI_MAIN:
      return "eevee_game_ssgi_main";
    case SH_SSGI_BLUR:
      return "eevee_game_ssgi_blur";

    case SH_BLOOM_DOWNSAMPLE:
      return "eevee_game_bloom_downsample";
    case SH_BLOOM_UPSAMPLE:
      return "eevee_game_bloom_upsample";
    case SH_BLOOM_COMPOSITE:
      return "eevee_game_bloom_composite";

    case SH_VOLUME_SCATTER:
      return "eevee_game_volume_scatter";
    case SH_VOLUME_INTEGRATE:
      return "eevee_game_volume_integrate";
    case SH_VOLUME_RESOLVE:
      return "eevee_game_volume_resolve";

    case SH_CULLING_COMPUTE:
      return "eevee_game_culling_compute";

    case SH_FXAA:
      return "eevee_game_fxaa";
    case SH_SMAA_EDGE:
      return "eevee_game_smaa_edge";
    case SH_SMAA_WEIGHT:
      return "eevee_game_smaa_weight";
    case SH_SMAA_BLEND:
      return "eevee_game_smaa_blend";

    case SH_DOF_COC_SETUP:
      return "eevee_game_dof_coc_setup";
    case SH_DOF_BOKEH_BLUR:
      return "eevee_game_dof_bokeh_blur";
    case SH_DOF_RESOLVE:
      return "eevee_game_dof_resolve";

    /* EEVEE's own info, used as is: the Hi-Z pyramid follows its layout. */
    case SH_HIZ_UPDATE:
      return "eevee_hiz_update";
    case SH_LIGHT_CULL:
      return "eevee_game_light_cull";
    case SH_FILM_PRESENT:
      return "eevee_game_film_present";

    default:
      BLI_assert_unreachable();
      return nullptr;
  }
}

void ShaderModule::static_shaders_load(ShaderGroups groups)
{
  /* Compile all shaders in the requested groups synchronously, so the cost
   * is paid at startup and not on first use in the middle of a frame.
   * Group membership is defined here; add new shaders to the appropriate
   * group when they are added to eShaderType. */
  struct ShaderGroupEntry {
    eShaderType type;
    ShaderGroups group;
  };

  static const ShaderGroupEntry entries[] = {
      /* Lighting */
      {SH_DEFERRED_LIGHT, SG_LIGHTING},
      {SH_LIGHT_CULL, SG_LIGHTING},

      /* Shadow: PCF compute filter */
      {SH_SHADOW_PCF_FILTER, SG_SHADOW},

      /* Post FX */
      {SH_BLOOM_DOWNSAMPLE, SG_POST_FX},
      {SH_BLOOM_UPSAMPLE, SG_POST_FX},
      {SH_BLOOM_COMPOSITE, SG_POST_FX},
      {SH_DOF_COC_SETUP, SG_POST_FX},
      {SH_DOF_BOKEH_BLUR, SG_POST_FX},
      {SH_DOF_RESOLVE, SG_POST_FX},
      {SH_VOLUME_SCATTER, SG_POST_FX},
      {SH_VOLUME_INTEGRATE, SG_POST_FX},
      {SH_VOLUME_RESOLVE, SG_POST_FX},
      {SH_FILM_PRESENT, SG_POST_FX},

      /* AA */
      {SH_FXAA, SG_AA},
      {SH_SMAA_EDGE, SG_AA},
      {SH_SMAA_WEIGHT, SG_AA},
      {SH_SMAA_BLEND, SG_AA},

      /* Screen space effects */
      {SH_SSR_TRACE, SG_REFLECT},
      {SH_GTAO_MAIN, SG_REFLECT},
      {SH_GTAO_UPSAMPLE, SG_REFLECT},
      {SH_SSGI_MAIN, SG_REFLECT},
      {SH_SSGI_BLUR, SG_REFLECT},

      /* Infrastructure (always compile) */
      {SH_HIZ_UPDATE, SG_LIGHTING},
      {SH_CULLING_COMPUTE, SG_LIGHTING},
  };

  for (const ShaderGroupEntry &e : entries) {
    if ((groups & e.group) == 0) {
      continue;
    }
    /* Null for types without a create info; nothing to compile. */
    static_shader_get(e.type);
  }
}

GPUMaterial *ShaderModule::material_shader_get(blender::Material *material,
                                                bNodeTree *ntree,
                                                eevee::eMaterialPipeline pipeline_type,
                                                eevee::eMaterialGeometry geometry_type,
                                                bool deferred,
                                                blender::Material *default_mat)
{
  eevee::ShaderModule *eevee_shaders = eevee::ShaderModule::module_get();
  if (pipeline_type != eevee::MAT_PIPE_FORWARD) {
    /* Depth, deferred and volume shaders are EEVEE's, unchanged. */
    return eevee_shaders->material_shader_get(
        material, ntree, pipeline_type, geometry_type, deferred, default_mat);
  }

  const uint64_t shader_uuid = eevee::shader_uuid_from_material_type(
                                   pipeline_type,
                                   geometry_type,
                                   eevee::to_displacement_type(material->displacement_method),
                                   eevee::to_thickness_type(material->thickness_mode),
                                   material->blend_flag) |
                               GAME_FORWARD_UUID_BIT;
  /* No pass replacement: EEVEE's replacement callback decodes the uuid and
   * would hand back its own default forward pass. */
  GPUMaterialFromNodeTreeResult result = GPU_material_from_nodetree(material,
                                                                    ntree,
                                                                    &material->gpumaterial,
                                                                    material->id.name,
                                                                    GPU_MAT_EEVEE,
                                                                    shader_uuid,
                                                                    deferred,
                                                                    game_forward_codegen,
                                                                    eevee_shaders,
                                                                    nullptr);
  return result.material;
}

} // namespace blender::eevee_game
