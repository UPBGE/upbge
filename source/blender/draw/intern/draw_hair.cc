/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2017 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup draw
 *
 * \brief Contains procedural GPU hair drawing methods.
 */

#include "DRW_render.h"

#include "BLI_string_utils.h"
#include "BLI_utildefines.h"

#include "DNA_collection_types.h"
#include "DNA_customdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_particle_types.h"

#include "BKE_duplilist.h"

#include "GPU_batch.h"
#include "GPU_capabilities.h"
#include "GPU_compute.h"
#include "GPU_material.h"
#include "GPU_shader.h"
#include "GPU_texture.h"
#include "GPU_vertex_buffer.h"

#include "DRW_gpu_wrapper.hh"

#include "draw_hair_private.h"
#include "draw_shader.h"
#include "draw_shader_shared.h"

#ifndef __APPLE__
#  define USE_TRANSFORM_FEEDBACK
#  define USE_COMPUTE_SHADERS
#endif

BLI_INLINE eParticleRefineShaderType drw_hair_shader_type_get()
{
#ifdef USE_COMPUTE_SHADERS
  if (GPU_compute_shader_support() && GPU_shader_storage_buffer_objects_support()) {
    return PART_REFINE_SHADER_COMPUTE;
  }
#endif
#ifdef USE_TRANSFORM_FEEDBACK
  return PART_REFINE_SHADER_TRANSFORM_FEEDBACK;
#endif
  return PART_REFINE_SHADER_TRANSFORM_FEEDBACK_WORKAROUND;
}

#ifndef USE_TRANSFORM_FEEDBACK
struct ParticleRefineCall {
  struct ParticleRefineCall *next;
  GPUVertBuf *vbo;
  DRWShadingGroup *shgrp;
  uint vert_len;
};

static ParticleRefineCall *g_tf_calls = nullptr;
static int g_tf_id_offset;
static int g_tf_target_width;
static int g_tf_target_height;
#endif

static GPUVertBuf *g_dummy_vbo = nullptr;
static GPUTexture *g_dummy_texture = nullptr;
static DRWPass *g_tf_pass; /* XXX can be a problem with multiple DRWManager in the future */
static blender::draw::UniformBuffer<CurvesInfos> *g_dummy_curves_info = nullptr;

static GPUShader *hair_refine_shader_get(ParticleRefineShader refinement)
{
  return DRW_shader_hair_refine_get(refinement, drw_hair_shader_type_get());
}

void DRW_hair_init(void)
{
#if defined(USE_TRANSFORM_FEEDBACK) || defined(USE_COMPUTE_SHADERS)
  g_tf_pass = DRW_pass_create("Update Hair Pass", DRW_STATE_NO_DRAW);
#else
  g_tf_pass = DRW_pass_create("Update Hair Pass", DRW_STATE_WRITE_COLOR);
#endif

  if (g_dummy_vbo == nullptr) {
    /* initialize vertex format */
    GPUVertFormat format = {0};
    uint dummy_id = GPU_vertformat_attr_add(&format, "dummy", GPU_COMP_F32, 4, GPU_FETCH_FLOAT);

    g_dummy_vbo = GPU_vertbuf_create_with_format(&format);

    const float vert[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    GPU_vertbuf_data_alloc(g_dummy_vbo, 1);
    GPU_vertbuf_attr_fill(g_dummy_vbo, dummy_id, vert);
    /* Create vbo immediately to bind to texture buffer. */
    GPU_vertbuf_use(g_dummy_vbo);

    g_dummy_texture = GPU_texture_create_from_vertbuf("hair_dummy_attr", g_dummy_vbo);

    g_dummy_curves_info = MEM_new<blender::draw::UniformBuffer<CurvesInfos>>(
        "g_dummy_curves_info");
    memset(g_dummy_curves_info->is_point_attribute,
           0,
           sizeof(g_dummy_curves_info->is_point_attribute));
    g_dummy_curves_info->push_update();
  }
}

static void drw_hair_particle_cache_shgrp_attach_resources(DRWShadingGroup *shgrp,
                                                           ParticleHairCache *cache,
                                                           const int subdiv)
{
  DRW_shgroup_uniform_texture(shgrp, "hairPointBuffer", cache->point_tex);
  DRW_shgroup_uniform_texture(shgrp, "hairStrandBuffer", cache->strand_tex);
  DRW_shgroup_uniform_texture(shgrp, "hairStrandSegBuffer", cache->strand_seg_tex);
  DRW_shgroup_uniform_int(shgrp, "hairStrandsRes", &cache->final[subdiv].strands_res, 1);
}

static void drw_hair_particle_cache_update_compute(ParticleHairCache *cache, const int subdiv)
{
  const int strands_len = cache->strands_len;
  const int final_points_len = cache->final[subdiv].strands_res * strands_len;
  if (final_points_len > 0) {
    GPUShader *shader = hair_refine_shader_get(PART_REFINE_CATMULL_ROM);
    DRWShadingGroup *shgrp = DRW_shgroup_create(shader, g_tf_pass);
    drw_hair_particle_cache_shgrp_attach_resources(shgrp, cache, subdiv);
    DRW_shgroup_vertex_buffer(shgrp, "posTime", cache->final[subdiv].proc_buf);

    const int max_strands_per_call = GPU_max_work_group_count(0);
    int strands_start = 0;
    while (strands_start < strands_len) {
      int batch_strands_len = MIN2(strands_len - strands_start, max_strands_per_call);
      DRWShadingGroup *subgroup = DRW_shgroup_create_sub(shgrp);
      DRW_shgroup_uniform_int_copy(subgroup, "hairStrandOffset", strands_start);
      DRW_shgroup_call_compute(subgroup, batch_strands_len, cache->final[subdiv].strands_res, 1);
      strands_start += batch_strands_len;
    }
  }
}

static void drw_hair_particle_cache_update_transform_feedback(ParticleHairCache *cache,
                                                              const int subdiv)
{
  const int final_points_len = cache->final[subdiv].strands_res * cache->strands_len;
  if (final_points_len > 0) {
    GPUShader *tf_shader = hair_refine_shader_get(PART_REFINE_CATMULL_ROM);

#ifdef USE_TRANSFORM_FEEDBACK
    DRWShadingGroup *tf_shgrp = DRW_shgroup_transform_feedback_create(
        tf_shader, g_tf_pass, cache->final[subdiv].proc_buf);
#else
    DRWShadingGroup *tf_shgrp = DRW_shgroup_create(tf_shader, g_tf_pass);

    ParticleRefineCall *pr_call = (ParticleRefineCall *)MEM_mallocN(sizeof(*pr_call), __func__);
    pr_call->next = g_tf_calls;
    pr_call->vbo = cache->final[subdiv].proc_buf;
    pr_call->shgrp = tf_shgrp;
    pr_call->vert_len = final_points_len;
    g_tf_calls = pr_call;
    DRW_shgroup_uniform_int(tf_shgrp, "targetHeight", &g_tf_target_height, 1);
    DRW_shgroup_uniform_int(tf_shgrp, "targetWidth", &g_tf_target_width, 1);
    DRW_shgroup_uniform_int(tf_shgrp, "idOffset", &g_tf_id_offset, 1);
#endif

    drw_hair_particle_cache_shgrp_attach_resources(tf_shgrp, cache, subdiv);
    DRW_shgroup_call_procedural_points(tf_shgrp, nullptr, final_points_len);
  }
}

static ParticleHairCache *drw_hair_particle_cache_get(Object *object,
                                                      ParticleSystem *psys,
                                                      ModifierData *md,
                                                      GPUMaterial *gpu_material,
                                                      int subdiv,
                                                      int thickness_res)
{
  ParticleHairCache *cache;
  bool update = particles_ensure_procedural_data(
      object, psys, md, &cache, gpu_material, subdiv, thickness_res);

  if (update) {
    if (drw_hair_shader_type_get() == PART_REFINE_SHADER_COMPUTE) {
      drw_hair_particle_cache_update_compute(cache, subdiv);
    }
    else {
      drw_hair_particle_cache_update_transform_feedback(cache, subdiv);
    }
  }
  return cache;
}

GPUVertBuf *DRW_hair_pos_buffer_get(Object *object, ParticleSystem *psys, ModifierData *md)
{
  const DRWContextState *draw_ctx = DRW_context_state_get();
  Scene *scene = draw_ctx->scene;

  int subdiv = scene->r.hair_subdiv;
  int thickness_res = (scene->r.hair_type == SCE_HAIR_SHAPE_STRAND) ? 1 : 2;

  ParticleHairCache *cache = drw_hair_particle_cache_get(
      object, psys, md, nullptr, subdiv, thickness_res);

  return cache->final[subdiv].proc_buf;
}

void DRW_hair_duplimat_get(Object *object,
                           ParticleSystem *UNUSED(psys),
                           ModifierData *UNUSED(md),
                           float (*dupli_mat)[4])
{
  Object *dupli_parent = DRW_object_get_dupli_parent(object);
  DupliObject *dupli_object = DRW_object_get_dupli(object);

  if ((dupli_parent != nullptr) && (dupli_object != nullptr)) {
    if (dupli_object->type & OB_DUPLICOLLECTION) {
      unit_m4(dupli_mat);
      Collection *collection = dupli_parent->instance_collection;
      if (collection != nullptr) {
        sub_v3_v3(dupli_mat[3], collection->instance_offset);
      }
      mul_m4_m4m4(dupli_mat, dupli_parent->obmat, dupli_mat);
    }
    else {
      copy_m4_m4(dupli_mat, dupli_object->ob->obmat);
      invert_m4(dupli_mat);
      mul_m4_m4m4(dupli_mat, object->obmat, dupli_mat);
    }
  }
  else {
    unit_m4(dupli_mat);
  }
}

DRWShadingGroup *DRW_shgroup_hair_create_sub(Object *object,
                                             ParticleSystem *psys,
                                             ModifierData *md,
                                             DRWShadingGroup *shgrp_parent,
                                             GPUMaterial *gpu_material)
{
  const DRWContextState *draw_ctx = DRW_context_state_get();
  Scene *scene = draw_ctx->scene;
  float dupli_mat[4][4];

  int subdiv = scene->r.hair_subdiv;
  int thickness_res = (scene->r.hair_type == SCE_HAIR_SHAPE_STRAND) ? 1 : 2;

  ParticleHairCache *hair_cache = drw_hair_particle_cache_get(
      object, psys, md, gpu_material, subdiv, thickness_res);

  DRWShadingGroup *shgrp = DRW_shgroup_create_sub(shgrp_parent);

  /* TODO: optimize this. Only bind the ones GPUMaterial needs. */
  for (int i = 0; i < hair_cache->num_uv_layers; i++) {
    for (int n = 0; n < MAX_LAYER_NAME_CT && hair_cache->uv_layer_names[i][n][0] != '\0'; n++) {
      DRW_shgroup_uniform_texture(shgrp, hair_cache->uv_layer_names[i][n], hair_cache->uv_tex[i]);
    }
  }
  for (int i = 0; i < hair_cache->num_col_layers; i++) {
    for (int n = 0; n < MAX_LAYER_NAME_CT && hair_cache->col_layer_names[i][n][0] != '\0'; n++) {
      DRW_shgroup_uniform_texture(
          shgrp, hair_cache->col_layer_names[i][n], hair_cache->col_tex[i]);
    }
  }

  /* Fix issue with certain driver not drawing anything if there is no texture bound to
   * "ac", "au", "u" or "c". */
  if (hair_cache->num_uv_layers == 0) {
    DRW_shgroup_uniform_texture(shgrp, "u", g_dummy_texture);
    DRW_shgroup_uniform_texture(shgrp, "au", g_dummy_texture);
  }
  if (hair_cache->num_col_layers == 0) {
    DRW_shgroup_uniform_texture(shgrp, "c", g_dummy_texture);
    DRW_shgroup_uniform_texture(shgrp, "ac", g_dummy_texture);
  }

  DRW_hair_duplimat_get(object, psys, md, dupli_mat);

  /* Get hair shape parameters. */
  ParticleSettings *part = psys->part;
  float hair_rad_shape = part->shape;
  float hair_rad_root = part->rad_root * part->rad_scale * 0.5f;
  float hair_rad_tip = part->rad_tip * part->rad_scale * 0.5f;
  bool hair_close_tip = (part->shape_flag & PART_SHAPE_CLOSE_TIP) != 0;

  DRW_shgroup_uniform_texture(shgrp, "hairPointBuffer", hair_cache->final[subdiv].proc_tex);
  if (hair_cache->length_tex) {
    DRW_shgroup_uniform_texture(shgrp, "l", hair_cache->length_tex);
  }

  DRW_shgroup_uniform_block(shgrp, "drw_curves", *g_dummy_curves_info);
  DRW_shgroup_uniform_int(shgrp, "hairStrandsRes", &hair_cache->final[subdiv].strands_res, 1);
  DRW_shgroup_uniform_int_copy(shgrp, "hairThicknessRes", thickness_res);
  DRW_shgroup_uniform_float_copy(shgrp, "hairRadShape", hair_rad_shape);
  DRW_shgroup_uniform_mat4_copy(shgrp, "hairDupliMatrix", dupli_mat);
  DRW_shgroup_uniform_float_copy(shgrp, "hairRadRoot", hair_rad_root);
  DRW_shgroup_uniform_float_copy(shgrp, "hairRadTip", hair_rad_tip);
  DRW_shgroup_uniform_bool_copy(shgrp, "hairCloseTip", hair_close_tip);
  /* TODO(fclem): Until we have a better way to cull the hair and render with orco, bypass
   * culling test. */
  GPUBatch *geom = hair_cache->final[subdiv].proc_hairs[thickness_res - 1];
  DRW_shgroup_call_no_cull(shgrp, geom, object);

  return shgrp;
}

void DRW_hair_update()
{
#ifndef USE_TRANSFORM_FEEDBACK
  /**
   * Workaround to transform feedback not working on mac.
   * On some system it crashes (see T58489) and on some other it renders garbage (see T60171).
   *
   * So instead of using transform feedback we render to a texture,
   * read back the result to system memory and re-upload as VBO data.
   * It is really not ideal performance wise, but it is the simplest
   * and the most local workaround that still uses the power of the GPU.
   */

  if (g_tf_calls == nullptr) {
    return;
  }

  /* Search ideal buffer size. */
  uint max_size = 0;
  for (ParticleRefineCall *pr_call = g_tf_calls; pr_call; pr_call = pr_call->next) {
    max_size = max_ii(max_size, pr_call->vert_len);
  }

  /* Create target Texture / Frame-buffer */
  /* Don't use max size as it can be really heavy and fail.
   * Do chunks of maximum 2048 * 2048 hair points. */
  int width = 2048;
  int height = min_ii(width, 1 + max_size / width);
  GPUTexture *tex = DRW_texture_pool_query_2d(
      width, height, GPU_RGBA32F, (DrawEngineType *)DRW_hair_update);
  g_tf_target_height = height;
  g_tf_target_width = width;

  GPUFrameBuffer *fb = nullptr;
  GPU_framebuffer_ensure_config(&fb,
                                {
                                    GPU_ATTACHMENT_NONE,
                                    GPU_ATTACHMENT_TEXTURE(tex),
                                });

  float *data = (float *)MEM_mallocN(sizeof(float[4]) * width * height, "tf fallback buffer");

  GPU_framebuffer_bind(fb);
  while (g_tf_calls != nullptr) {
    ParticleRefineCall *pr_call = g_tf_calls;
    g_tf_calls = g_tf_calls->next;

    g_tf_id_offset = 0;
    while (pr_call->vert_len > 0) {
      int max_read_px_len = min_ii(width * height, pr_call->vert_len);

      DRW_draw_pass_subset(g_tf_pass, pr_call->shgrp, pr_call->shgrp);
      /* Read back result to main memory. */
      GPU_framebuffer_read_color(fb, 0, 0, width, height, 4, 0, GPU_DATA_FLOAT, data);
      /* Upload back to VBO. */
      GPU_vertbuf_use(pr_call->vbo);
      GPU_vertbuf_update_sub(pr_call->vbo,
                             sizeof(float[4]) * g_tf_id_offset,
                             sizeof(float[4]) * max_read_px_len,
                             data);

      g_tf_id_offset += max_read_px_len;
      pr_call->vert_len -= max_read_px_len;
    }

    MEM_freeN(pr_call);
  }

  MEM_freeN(data);
  GPU_framebuffer_free(fb);
#else
  /* Just render the pass when using compute shaders or transform feedback. */
  DRW_draw_pass(g_tf_pass);
  if (drw_hair_shader_type_get() == PART_REFINE_SHADER_COMPUTE) {
    GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  }
#endif
}

void DRW_hair_free(void)
{
  GPU_VERTBUF_DISCARD_SAFE(g_dummy_vbo);
  DRW_TEXTURE_FREE_SAFE(g_dummy_texture);
  MEM_delete(g_dummy_curves_info);
}
