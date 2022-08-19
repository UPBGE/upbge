/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2022 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup draw
 *
 * \brief Contains Volume object GPU attributes configuration.
 */

#include "DRW_gpu_wrapper.hh"
#include "DRW_render.h"

#include "DNA_fluid_types.h"
#include "DNA_volume_types.h"

#include "BKE_fluid.h"
#include "BKE_global.h"
#include "BKE_mesh.h"
#include "BKE_modifier.h"
#include "BKE_volume.h"
#include "BKE_volume_render.h"

#include "GPU_material.h"

#include "draw_common.h"
#include "draw_manager.h"

using namespace blender;
using namespace blender::draw;
using VolumeInfosBuf = blender::draw::UniformBuffer<VolumeInfos>;

static struct {
  GPUTexture *dummy_zero;
  GPUTexture *dummy_one;
  float dummy_grid_mat[4][4];
} g_data = {};

struct VolumeUniformBufPool {
  Vector<VolumeInfosBuf *> ubos;
  uint used = 0;

  ~VolumeUniformBufPool()
  {
    for (VolumeInfosBuf *ubo : ubos) {
      delete ubo;
    }
  }

  void reset()
  {
    used = 0;
  }

  VolumeInfosBuf *alloc()
  {
    if (used >= ubos.size()) {
      VolumeInfosBuf *buf = new VolumeInfosBuf();
      ubos.append(buf);
      return buf;
    }
    return ubos[used++];
  }
};

void DRW_volume_ubos_pool_free(void *pool)
{
  delete reinterpret_cast<VolumeUniformBufPool *>(pool);
}

static void drw_volume_globals_init()
{
  const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  g_data.dummy_zero = GPU_texture_create_3d(
      "dummy_zero", 1, 1, 1, 1, GPU_RGBA8, GPU_DATA_FLOAT, zero);
  g_data.dummy_one = GPU_texture_create_3d(
      "dummy_one", 1, 1, 1, 1, GPU_RGBA8, GPU_DATA_FLOAT, one);
  GPU_texture_wrap_mode(g_data.dummy_zero, true, true);
  GPU_texture_wrap_mode(g_data.dummy_one, true, true);

  memset(g_data.dummy_grid_mat, 0, sizeof(g_data.dummy_grid_mat));
}

void DRW_volume_free(void)
{
  GPU_TEXTURE_FREE_SAFE(g_data.dummy_zero);
  GPU_TEXTURE_FREE_SAFE(g_data.dummy_one);
}

static GPUTexture *grid_default_texture(eGPUDefaultValue default_value)
{
  if (g_data.dummy_one == nullptr) {
    drw_volume_globals_init();
  }

  switch (default_value) {
    case GPU_DEFAULT_0:
      return g_data.dummy_zero;
    case GPU_DEFAULT_1:
      return g_data.dummy_one;
  }
  return g_data.dummy_zero;
}

void DRW_volume_init(DRWData *drw_data)
{
  if (drw_data->volume_grids_ubos == nullptr) {
    drw_data->volume_grids_ubos = new VolumeUniformBufPool();
  }
  VolumeUniformBufPool *pool = (VolumeUniformBufPool *)drw_data->volume_grids_ubos;
  pool->reset();

  if (g_data.dummy_one == nullptr) {
    drw_volume_globals_init();
  }
}

static DRWShadingGroup *drw_volume_object_grids_init(Object *ob,
                                                     ListBase *attrs,
                                                     DRWShadingGroup *grp)
{
  VolumeUniformBufPool *pool = (VolumeUniformBufPool *)DST.vmempool->volume_grids_ubos;
  VolumeInfosBuf &volume_infos = *pool->alloc();

  Volume *volume = (Volume *)ob->data;
  BKE_volume_load(volume, G.main);

  grp = DRW_shgroup_create_sub(grp);

  volume_infos.density_scale = BKE_volume_density_scale(volume, ob->obmat);
  volume_infos.color_mul = float4(1.0f);
  volume_infos.temperature_mul = 1.0f;
  volume_infos.temperature_bias = 0.0f;

  /* Bind volume grid textures. */
  int grid_id = 0, grids_len = 0;
  LISTBASE_FOREACH (GPUMaterialAttribute *, attr, attrs) {
    const VolumeGrid *volume_grid = BKE_volume_grid_find_for_read(volume, attr->name);
    const DRWVolumeGrid *drw_grid = (volume_grid) ?
                                        DRW_volume_batch_cache_get_grid(volume, volume_grid) :
                                        nullptr;
    /* Count number of valid attributes. */
    grids_len += int(volume_grid != nullptr);

    /* Handle 3 cases here:
     * - Grid exists and texture was loaded -> use texture.
     * - Grid exists but has zero size or failed to load -> use zero.
     * - Grid does not exist -> use default value. */
    const GPUTexture *grid_tex = (drw_grid)    ? drw_grid->texture :
                                 (volume_grid) ? g_data.dummy_zero :
                                                 grid_default_texture(attr->default_value);
    DRW_shgroup_uniform_texture(grp, attr->input_name, grid_tex);

    copy_m4_m4(volume_infos.grids_xform[grid_id++].ptr(),
               (drw_grid) ? drw_grid->object_to_texture : g_data.dummy_grid_mat);
  }
  /* Render nothing if there is no attribute for the shader to render.
   * This also avoids an assert caused by the bounding box being zero in size. */
  if (grids_len == 0) {
    return nullptr;
  }

  volume_infos.push_update();

  DRW_shgroup_uniform_block(grp, "drw_volume", volume_infos);

  return grp;
}

static DRWShadingGroup *drw_volume_object_mesh_init(Scene *scene,
                                                    Object *ob,
                                                    ListBase *attrs,
                                                    DRWShadingGroup *grp)
{
  VolumeUniformBufPool *pool = (VolumeUniformBufPool *)DST.vmempool->volume_grids_ubos;
  VolumeInfosBuf &volume_infos = *pool->alloc();

  ModifierData *md = nullptr;

  volume_infos.density_scale = 1.0f;
  volume_infos.color_mul = float4(1.0f);
  volume_infos.temperature_mul = 1.0f;
  volume_infos.temperature_bias = 0.0f;

  /* Smoke Simulation */
  if ((md = BKE_modifiers_findby_type(ob, eModifierType_Fluid)) &&
      (BKE_modifier_is_enabled(scene, md, eModifierMode_Realtime)) &&
      ((FluidModifierData *)md)->domain != nullptr) {
    FluidModifierData *fmd = (FluidModifierData *)md;
    FluidDomainSettings *fds = fmd->domain;

    /* Don't try to show liquid domains here. */
    if (!fds->fluid || !(fds->type == FLUID_DOMAIN_TYPE_GAS)) {
      return nullptr;
    }

    if (fds->fluid && (fds->type == FLUID_DOMAIN_TYPE_GAS)) {
      DRW_smoke_ensure(fmd, fds->flags & FLUID_DOMAIN_USE_NOISE);
    }

    grp = DRW_shgroup_create_sub(grp);

    int grid_id = 0;
    LISTBASE_FOREACH (GPUMaterialAttribute *, attr, attrs) {
      if (STREQ(attr->name, "density")) {
        DRW_shgroup_uniform_texture_ref(
            grp, attr->input_name, fds->tex_density ? &fds->tex_density : &g_data.dummy_one);
      }
      else if (STREQ(attr->name, "color")) {
        DRW_shgroup_uniform_texture_ref(
            grp, attr->input_name, fds->tex_color ? &fds->tex_color : &g_data.dummy_one);
      }
      else if (STR_ELEM(attr->name, "flame", "temperature")) {
        DRW_shgroup_uniform_texture_ref(
            grp, attr->input_name, fds->tex_flame ? &fds->tex_flame : &g_data.dummy_zero);
      }
      else {
        DRW_shgroup_uniform_texture(
            grp, attr->input_name, grid_default_texture(attr->default_value));
      }
      copy_m4_m4(volume_infos.grids_xform[grid_id++].ptr(), g_data.dummy_grid_mat);
    }

    bool use_constant_color = ((fds->active_fields & FLUID_DOMAIN_ACTIVE_COLORS) == 0 &&
                               (fds->active_fields & FLUID_DOMAIN_ACTIVE_COLOR_SET) != 0);
    if (use_constant_color) {
      volume_infos.color_mul = float4(UNPACK3(fds->active_color), 1.0f);
    }

    /* Output is such that 0..1 maps to 0..1000K */
    volume_infos.temperature_mul = fds->flame_max_temp - fds->flame_ignition;
    volume_infos.temperature_bias = fds->flame_ignition;
  }
  else {
    grp = DRW_shgroup_create_sub(grp);

    int grid_id = 0;
    LISTBASE_FOREACH (GPUMaterialAttribute *, attr, attrs) {
      DRW_shgroup_uniform_texture(
          grp, attr->input_name, grid_default_texture(attr->default_value));
      copy_m4_m4(volume_infos.grids_xform[grid_id++].ptr(), g_data.dummy_grid_mat);
    }
  }

  volume_infos.push_update();

  DRW_shgroup_uniform_block(grp, "drw_volume", volume_infos);

  return grp;
}

static DRWShadingGroup *drw_volume_world_grids_init(ListBase *attrs, DRWShadingGroup *grp)
{
  /* Bind default volume grid textures. */
  LISTBASE_FOREACH (GPUMaterialAttribute *, attr, attrs) {
    DRW_shgroup_uniform_texture(grp, attr->input_name, grid_default_texture(attr->default_value));
  }
  return grp;
}

DRWShadingGroup *DRW_shgroup_volume_create_sub(Scene *scene,
                                               Object *ob,
                                               DRWShadingGroup *shgrp,
                                               GPUMaterial *gpu_material)
{
  ListBase attrs = GPU_material_attributes(gpu_material);

  if (ob == nullptr) {
    return drw_volume_world_grids_init(&attrs, shgrp);
  }
  if (ob->type == OB_VOLUME) {
    return drw_volume_object_grids_init(ob, &attrs, shgrp);
  }
  return drw_volume_object_mesh_init(scene, ob, &attrs, shgrp);
}
