/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_time.h"

#include "BLI_math_vector.h" /* copy_v3_fl() */
#include "BKE_lib_id.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_image.hh"
#include "NOD_shader.h"
#include "DNA_material_types.h"

#include "GPU_material.hh"

#include "BLI_listbase.h"
#include "BLI_task.hh"

#include "eevee_game_instance.hh"
#include "eevee_game_material.hh"

/* Reuse EEVEE's shader_uuid helpers and ShaderKey/MaterialKey logic directly. */
#include "../eevee/eevee_material.hh"

namespace blender::eevee_game {

using namespace eevee;

/* ================================================================
 * Constructor / Destructor
 * ================================================================ */

MaterialModule::MaterialModule(GameInstance &inst) : inst_(inst)
{
  /* Build default materials using node trees. These are the fallback used
   * while a material's shader is still compiling asynchronously. */
  {
    diffuse_mat = BKE_id_new_nomain<blender::Material>("EEVEE_GAME default diffuse");
    bNodeTree *ntree = diffuse_mat->nodetree;
    diffuse_mat->surface_render_method = MA_SURFACE_METHOD_FORWARD;

    bNode *bsdf   = bke::node_add_static_node(nullptr, *ntree, SH_NODE_BSDF_DIFFUSE);
    bNodeSocket *color = bke::node_find_socket(*bsdf, SOCK_IN, "Color");
    /* 0.18 = middle grey, standard VFX reference reflectance */
    copy_v3_fl((static_cast<bNodeSocketValueRGBA *>(color->default_value))->value, 0.18f);

    bNode *output = bke::node_add_static_node(nullptr, *ntree, SH_NODE_OUTPUT_MATERIAL);
    bke::node_add_link(*ntree,
                       *bsdf,   *bke::node_find_socket(*bsdf,   SOCK_OUT, "BSDF"),
                       *output, *bke::node_find_socket(*output, SOCK_IN,  "Surface"));
    bke::node_set_active(*ntree, *output);
  }
  {
    metallic_mat = BKE_id_new_nomain<blender::Material>("EEVEE_GAME default metallic");
    bNodeTree *ntree = metallic_mat->nodetree;
    metallic_mat->surface_render_method = MA_SURFACE_METHOD_FORWARD;

    bNode *bsdf      = bke::node_add_static_node(nullptr, *ntree, SH_NODE_BSDF_GLOSSY);
    bNodeSocket *color     = bke::node_find_socket(*bsdf, SOCK_IN, "Color");
    bNodeSocket *roughness = bke::node_find_socket(*bsdf, SOCK_IN, "Roughness");
    copy_v3_fl((static_cast<bNodeSocketValueRGBA *>(color->default_value))->value, 1.0f);
    (static_cast<bNodeSocketValueFloat *>(roughness->default_value))->value = 0.0f;

    bNode *output = bke::node_add_static_node(nullptr, *ntree, SH_NODE_OUTPUT_MATERIAL);
    bke::node_add_link(*ntree,
                       *bsdf,   *bke::node_find_socket(*bsdf,   SOCK_OUT, "BSDF"),
                       *output, *bke::node_find_socket(*output, SOCK_IN,  "Surface"));
    bke::node_set_active(*ntree, *output);
  }
  {
    /* Copy Blender's built-in default materials so we own the pointers */
    default_surface = reinterpret_cast<blender::Material *>(BKE_id_copy_ex(
        nullptr, &BKE_material_default_surface()->id, nullptr, LIB_ID_COPY_LOCALIZE));
    default_volume = reinterpret_cast<blender::Material *>(BKE_id_copy_ex(
        nullptr, &BKE_material_default_volume()->id, nullptr, LIB_ID_COPY_LOCALIZE));
  }
  {
    /* Bright magenta emission: immediately visible so shader errors are obvious */
    error_mat_ = BKE_id_new_nomain<blender::Material>("EEVEE_GAME error");
    bNodeTree *ntree = error_mat_->nodetree;

    bNode *bsdf  = bke::node_add_static_node(nullptr, *ntree, SH_NODE_EMISSION);
    bNodeSocket *color = bke::node_find_socket(*bsdf, SOCK_IN, "Color");
    copy_v3_fl3((static_cast<bNodeSocketValueRGBA *>(color->default_value))->value,
                1.0f, 0.0f, 1.0f);

    bNode *output = bke::node_add_static_node(nullptr, *ntree, SH_NODE_OUTPUT_MATERIAL);
    bke::node_add_link(*ntree,
                       *bsdf,   *bke::node_find_socket(*bsdf,   SOCK_OUT, "Emission"),
                       *output, *bke::node_find_socket(*output, SOCK_IN,  "Surface"));
    bke::node_set_active(*ntree, *output);
  }
}

MaterialModule::~MaterialModule()
{
  BKE_id_free(nullptr, diffuse_mat);
  BKE_id_free(nullptr, metallic_mat);
  BKE_id_free(nullptr, default_surface);
  BKE_id_free(nullptr, default_volume);
  BKE_id_free(nullptr, error_mat_);
}

/* ================================================================
 * begin_sync / end_sync
 * ================================================================ */

void MaterialModule::begin_sync()
{
  queued_shaders_count          = 0;
  queued_textures_count         = 0;
  queued_optimize_shaders_count = 0;

  /* Respect ViewLayer material override if set */
  material_override = inst_.view_layer ?
      DEG_get_evaluated(inst_.depsgraph, inst_.view_layer->mat_override) :
      nullptr;

  /* Snapshot the global GPU pass compilation counter so we can detect
   * newly compiled shaders this frame and reset FSR temporal accumulation. */
  uint64_t next_update    = GPU_pass_global_compilation_count();
  gpu_pass_last_update_   = gpu_pass_next_update_;
  gpu_pass_next_update_   = next_update;

  texture_loading_queue_.clear();
  material_map_.clear();
  shader_map_.clear();
}

void MaterialModule::end_sync()
{
  if (texture_loading_queue_.is_empty()) {
    return;
  }

  GPU_debug_group_begin("Texture Loading");

  /* Load image data from disk in parallel threads */
  threading::parallel_for(texture_loading_queue_.index_range(), 1, [&](const IndexRange range) {
    for (auto i : range) {
      GPUMaterialTexture *tex = texture_loading_queue_[i];
      ImageUser *iuser = tex->iuser_available ? &tex->iuser : nullptr;
      BKE_image_get_tile(tex->ima, 0);
      threading::isolate_task([&]() {
        ImBuf *ibuf = BKE_image_acquire_ibuf(tex->ima, iuser, nullptr);
        BKE_image_release_ibuf(tex->ima, ibuf, nullptr);
      });
    }
  });

  /* Tag is not thread-safe; must be done on the main thread */
  for (GPUMaterialTexture *tex : texture_loading_queue_) {
    BKE_image_tag_time(tex->ima);
  }

  /* GPU upload: requires a valid GL/Vulkan context — not parallelisable */
  for (GPUMaterialTexture *tex : texture_loading_queue_) {
    BLI_assert(tex->ima);
    GPU_debug_group_begin(tex->ima->id.name);

    const bool use_tile_mapping = tex->tiled_mapping_name[0];
    ImageUser *iuser = tex->iuser_available ? &tex->iuser : nullptr;
    ImageGPUTextures gputex = BKE_image_get_gpu_material_texture(
        tex->ima, iuser, use_tile_mapping);

    inst_.manager->acquire_texture(*gputex.texture);
    if (gputex.tile_mapping) {
      inst_.manager->acquire_texture(*gputex.tile_mapping);
    }
    GPU_debug_group_end();
  }

  GPU_debug_group_end();
  texture_loading_queue_.clear();
}

/* ================================================================
 * queue_texture_loading
 * ================================================================ */

void MaterialModule::queue_texture_loading(GPUMaterial *gpumat)
{
  ListBaseT<GPUMaterialTexture> textures = GPU_material_textures(gpumat);
  for (GPUMaterialTexture *tex : ListBaseWrapper<GPUMaterialTexture>(textures)) {
    if (tex->ima) {
      const bool use_tile_mapping = tex->tiled_mapping_name[0];
      ImageUser *iuser = tex->iuser_available ? &tex->iuser : nullptr;
      ImageGPUTextures gputex = BKE_image_get_gpu_material_texture_try(
          tex->ima, iuser, use_tile_mapping);
      if (*gputex.texture == nullptr) {
        texture_loading_queue_.append(tex);
      }
    }
  }
}

/* ================================================================
 * material_pass_get
 *
 * Adapted from eevee::MaterialModule::material_pass_get().
 * Key differences:
 *   - No probe_capture parameter (no lightprobe passes in eevee_game).
 *   - Delegates to inst_.shaders.material_shader_get() which calls
 *     the same EEVEE GPU material backend.
 *   - inst_.pipelines.material_add() creates the sub-pass in the
 *     appropriate PassMain (prepass, gbuffer, shadow, forward).
 * ================================================================ */

MaterialPass MaterialModule::material_pass_get(Object *ob,
                                               blender::Material *blender_mat,
                                               eMaterialPipeline pipeline_type,
                                               eMaterialGeometry geometry_type)
{
  bNodeTree *ntree = blender_mat->nodetree ? blender_mat->nodetree :
                                             default_surface->nodetree;

  /* In game (viewport) mode we always use deferred compilation so the
   * frame loop is never stalled by shader compilation. */
  const bool use_deferred = true;

  const bool is_volume = ELEM(pipeline_type,
                              MAT_PIPE_VOLUME_OCCUPANCY,
                              MAT_PIPE_VOLUME_MATERIAL);
  blender::Material *default_mat = is_volume ? default_volume : default_surface;

  MaterialPass matpass;
  matpass.gpumat = inst_.shaders.material_shader_get(
      blender_mat, ntree, pipeline_type, geometry_type, use_deferred, default_mat);

  queue_texture_loading(matpass.gpumat);

  const bool is_forward = ELEM(pipeline_type,
                               MAT_PIPE_FORWARD,
                               MAT_PIPE_PREPASS_FORWARD,
                               MAT_PIPE_PREPASS_FORWARD_VELOCITY,
                               MAT_PIPE_PREPASS_OVERLAP);

  switch (GPU_material_status(matpass.gpumat)) {
    case GPU_MAT_SUCCESS: {
      if (GPU_material_optimization_status(matpass.gpumat) == GPU_MAT_OPTIMIZATION_QUEUED) {
        queued_optimize_shaders_count++;
      }
      break;
    }
    case GPU_MAT_QUEUED:
      queued_shaders_count++;
      /* Fall back to default material while the real one compiles */
      matpass.gpumat = inst_.shaders.material_shader_get(
          default_mat, default_mat->nodetree, pipeline_type, geometry_type, false, nullptr);
      break;
    case GPU_MAT_FAILED:
    default:
      matpass.gpumat = inst_.shaders.material_shader_get(
          error_mat_, error_mat_->nodetree, pipeline_type, geometry_type, false, nullptr);
      break;
  }
  BLI_assert(GPU_material_status(matpass.gpumat) == GPU_MAT_SUCCESS);

  inst_.manager->register_layer_attributes(matpass.gpumat);

  /* If a newly compiled shader appeared this frame, notify FSR to discard
   * temporal history to avoid ghosting on the newly-lit geometry. */
  const bool pass_updated =
      GPU_material_compilation_timestamp(matpass.gpumat) > gpu_pass_last_update_;
  if (pass_updated) {
    inst_.upscale.notify_camera_cut();
  }

  /* Volume passes and transparent forward passes don't have a fixed sub-pass;
   * they create one per-object in material_sync() below. */
  if (is_volume || (is_forward && GPU_material_flag_get(matpass.gpumat, GPU_MATFLAG_TRANSPARENT))) {
    matpass.sub_pass = nullptr;
    return matpass;
  }

  /* Bin by shader key to avoid unnecessary shader switches in the draw list.
   * ShaderKey packs the gpu::Shader pointer + closure bits + blend flags into a uint64. */
  ShaderKey shader_key(matpass.gpumat, blender_mat, MAT_PROBE_NONE);
  const uint64_t key_hash = shader_key.hash();

  PassMain::Sub *shader_sub = shader_map_.lookup_or_add_cb(key_hash, [&]() {
    /* First object using this shader: create the sub-pass bucket in the pipeline */
    return inst_.pipelines.material_add(ob, blender_mat, matpass.gpumat, pipeline_type);
  });

  if (shader_sub != nullptr) {
    /* Each material gets its own sub of the shader sub so per-material uniforms
     * (color, roughness, etc.) are isolated from other materials using the same shader. */
    matpass.sub_pass = &shader_sub->sub(GPU_material_get_name(matpass.gpumat));
    matpass.sub_pass->material_set(*inst_.manager, matpass.gpumat, true,
                                   inst_.anisotropic_filtering);
  }
  else {
    matpass.sub_pass = nullptr;
  }

  return matpass;
}

/* ================================================================
 * material_sync
 *
 * Builds or retrieves the full Material for one (object, blender_mat) pair.
 * Adapted from eevee::MaterialModule::material_sync() with the following
 * simplifications for eevee_game:
 *   - No lightprobe sphere / planar probe passes.
 *   - No baking / capture passes.
 *   - Transparent forward objects create per-object sub-passes via
 *     pipelines.forward.material_transparent_add() as in EEVEE.
 * ================================================================ */

Material &MaterialModule::material_sync(Object *ob,
                                         blender::Material *blender_mat,
                                         eMaterialGeometry geometry_type,
                                         bool has_motion)
{
  const bool hide_on_camera = ob->visibility_flag & OB_HIDE_CAMERA;

  /* Volume objects */
  if (geometry_type == MAT_GEOM_VOLUME) {
    /* Key: pack geometry + volume pipeline + visibility into a single uint64 */
    MaterialKey mat_key(blender_mat, geometry_type, MAT_PIPE_VOLUME_MATERIAL,
                        ob->visibility_flag);
    Material &mat = material_map_.lookup_or_add_cb(mat_key.hash(), [&]() {
      Material m = {};
      m.volume_occupancy = material_pass_get(ob, blender_mat,
                                             MAT_PIPE_VOLUME_OCCUPANCY, MAT_GEOM_VOLUME);
      m.volume_material  = material_pass_get(ob, blender_mat,
                                             MAT_PIPE_VOLUME_MATERIAL,  MAT_GEOM_VOLUME);
      m.has_volume   = true;
      m.has_surface  = false;
      return m;
    });

    if (hide_on_camera) {
      mat.volume_occupancy.sub_pass = nullptr;
      mat.volume_material.sub_pass  = nullptr;
    }
    return mat;
  }

  /* Surface objects */
  const bool use_forward = (blender_mat->surface_render_method == MA_SURFACE_METHOD_FORWARD);

  eMaterialPipeline surface_pipe, prepass_pipe;
  if (use_forward) {
    surface_pipe = MAT_PIPE_FORWARD;
    prepass_pipe = has_motion ? MAT_PIPE_PREPASS_FORWARD_VELOCITY : MAT_PIPE_PREPASS_FORWARD;
  }
  else {
    surface_pipe = MAT_PIPE_DEFERRED;
    prepass_pipe = has_motion ? MAT_PIPE_PREPASS_DEFERRED_VELOCITY : MAT_PIPE_PREPASS_DEFERRED;
  }

  MaterialKey mat_key(blender_mat, geometry_type, surface_pipe, ob->visibility_flag);

  Material &mat = material_map_.lookup_or_add_cb(mat_key.hash(), [&]() {
    Material m = {};

    /* Depth/velocity prepass */
    m.prepass = hide_on_camera ? MaterialPass() :
                                 material_pass_get(ob, blender_mat, prepass_pipe, geometry_type);

    /* Main surface shading */
    m.shading = material_pass_get(ob, blender_mat, surface_pipe, geometry_type);
    if (hide_on_camera) {
      /* Keep gpumat for GPU_material API queries, but suppress drawing */
      m.shading.sub_pass = nullptr;
    }

    /* No dedicated shadow pass: ShadowModule draws the opaque depth prepass
     * from each light (see ShadowModule::render_tile). MAT_PIPE_SHADOW would
     * compile EEVEE's virtual shadow map shader, which writes through page
     * tables this engine does not have.
     * Limitation: per-object shadow visibility (OB_HIDE_SHADOW, or shadow
     * only objects hidden from the camera) is not honoured yet. */
    m.shadow = MaterialPass();

    m.has_surface = GPU_material_has_surface_output(m.shading.gpumat);
    m.has_volume  = GPU_material_has_volume_output(m.shading.gpumat);

    /* Volume embedded in a mesh material (e.g. glass with interior scatter) */
    if (m.has_volume && !hide_on_camera) {
      m.volume_occupancy = material_pass_get(ob, blender_mat,
                                             MAT_PIPE_VOLUME_OCCUPANCY, geometry_type);
      m.volume_material  = material_pass_get(ob, blender_mat,
                                             MAT_PIPE_VOLUME_MATERIAL,  geometry_type);
    }

    const bool is_transparent = GPU_material_flag_get(m.shading.gpumat, GPU_MATFLAG_TRANSPARENT);
    m.is_alpha_blend_transparent = use_forward && is_transparent;
    m.has_transparent_shadows    = (blender_mat->blend_flag & MA_BL_TRANSPARENT_SHADOW) &&
                                   is_transparent;
    return m;
  });

  /* Transparent objects need per-object sub-passes so the draw list can sort them
   * back-to-front. This must be done outside the lookup_or_add_cb (run every sync). */
  if (mat.is_alpha_blend_transparent && !hide_on_camera) {
    mat.overlap_masking.sub_pass = inst_.pipelines.prepass_transparent_add(
        ob, blender_mat, mat.shading.gpumat);
    mat.shading.sub_pass = inst_.pipelines.material_transparent_add(
        ob, blender_mat, mat.shading.gpumat);
  }

  return mat;
}

/* ================================================================
 * material_from_slot
 * ================================================================ */

blender::Material *MaterialModule::material_from_slot(Object *ob, int slot)
{
  blender::Material *ma = BKE_object_material_get_eval(ob, slot + 1);
  if (ma == nullptr) {
    return (ob->type == OB_VOLUME) ? BKE_material_default_volume() :
                                     BKE_material_default_surface();
  }
  return ma;
}

/* ================================================================
 * material_array_get
 * ================================================================ */

MaterialArray &MaterialModule::material_array_get(Object *ob, bool has_motion)
{
  material_array_.materials.clear();
  material_array_.gpu_materials.clear();

  const int mat_count = BKE_object_material_used_with_fallback_eval(*ob);
  for (auto i : IndexRange(mat_count)) {
    blender::Material *blender_mat = material_override ?
                                         material_override :
                                         material_from_slot(ob, i);
    Material &mat = material_sync(ob, blender_mat, to_material_geometry(ob), has_motion);
    /* Copy the whole struct: material_sync() may reallocate its container
     * on the next call, invalidating references. */
    material_array_.materials.append(mat);
    material_array_.gpu_materials.append(mat.shading.gpumat);
  }
  return material_array_;
}

/* ================================================================
 * material_get  (single slot — used by curves, volumes, etc.)
 * ================================================================ */

Material &MaterialModule::material_get(Object *ob,
                                        bool has_motion,
                                        int mat_nr,
                                        eMaterialGeometry geometry_type)
{
  blender::Material *blender_mat = material_override ? material_override :
                                                       material_from_slot(ob, mat_nr);
  return material_sync(ob, blender_mat, geometry_type, has_motion);
}

} // namespace blender::eevee_game
