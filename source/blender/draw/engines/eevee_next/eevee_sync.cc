/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 Blender Foundation.
 */

/** \file
 * \ingroup eevee
 *
 * Converts the different renderable object types to drawcalls.
 */

#include "eevee_engine.h"

#include "BKE_gpencil.h"
#include "BKE_object.h"
#include "DEG_depsgraph_query.h"
#include "DNA_curves_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_modifier_types.h"
#include "DNA_particle_types.h"

#include "eevee_instance.hh"

namespace blender::eevee {

/* -------------------------------------------------------------------- */
/** \name Draw Data
 *
 * \{ */

static void draw_data_init_cb(struct DrawData *dd)
{
  /* Object has just been created or was never evaluated by the engine. */
  dd->recalc = ID_RECALC_ALL;
}

ObjectHandle &SyncModule::sync_object(Object *ob)
{
  DrawEngineType *owner = (DrawEngineType *)&DRW_engine_viewport_eevee_next_type;
  struct DrawData *dd = DRW_drawdata_ensure(
      (ID *)ob, owner, sizeof(eevee::ObjectHandle), draw_data_init_cb, nullptr);
  ObjectHandle &eevee_dd = *reinterpret_cast<ObjectHandle *>(dd);

  if (eevee_dd.object_key.ob == nullptr) {
    eevee_dd.object_key = ObjectKey(ob);
  }

  const int recalc_flags = ID_RECALC_COPY_ON_WRITE | ID_RECALC_TRANSFORM | ID_RECALC_SHADING |
                           ID_RECALC_GEOMETRY;
  if ((eevee_dd.recalc & recalc_flags) != 0) {
    inst_.sampling.reset();
    UNUSED_VARS(inst_);
  }

  return eevee_dd;
}

WorldHandle &SyncModule::sync_world(::World *world)
{
  DrawEngineType *owner = (DrawEngineType *)&DRW_engine_viewport_eevee_next_type;
  struct DrawData *dd = DRW_drawdata_ensure(
      (ID *)world, owner, sizeof(eevee::WorldHandle), draw_data_init_cb, nullptr);
  WorldHandle &eevee_dd = *reinterpret_cast<WorldHandle *>(dd);

  const int recalc_flags = ID_RECALC_ALL;
  if ((eevee_dd.recalc & recalc_flags) != 0) {
    inst_.sampling.reset();
  }
  return eevee_dd;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Common
 * \{ */

static inline void shgroup_geometry_call(DRWShadingGroup *grp,
                                         Object *ob,
                                         GPUBatch *geom,
                                         int v_first = -1,
                                         int v_count = -1,
                                         bool use_instancing = false)
{
  if (grp == nullptr) {
    return;
  }

  if (v_first == -1) {
    DRW_shgroup_call(grp, geom, ob);
  }
  else if (use_instancing) {
    DRW_shgroup_call_instance_range(grp, ob, geom, v_first, v_count);
  }
  else {
    DRW_shgroup_call_range(grp, ob, geom, v_first, v_count);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mesh
 * \{ */

void SyncModule::sync_mesh(Object *ob, ObjectHandle &ob_handle)
{
  bool has_motion = inst_.velocity.step_object_sync(ob, ob_handle.object_key, ob_handle.recalc);

  MaterialArray &material_array = inst_.materials.material_array_get(ob, has_motion);

  GPUBatch **mat_geom = DRW_cache_object_surface_material_get(
      ob, material_array.gpu_materials.data(), material_array.gpu_materials.size());

  if (mat_geom == nullptr) {
    return;
  }

  bool is_shadow_caster = false;
  bool is_alpha_blend = false;
  for (auto i : material_array.gpu_materials.index_range()) {
    GPUBatch *geom = mat_geom[i];
    if (geom == nullptr) {
      continue;
    }
    Material *material = material_array.materials[i];
    shgroup_geometry_call(material->shading.shgrp, ob, geom);
    shgroup_geometry_call(material->prepass.shgrp, ob, geom);
    shgroup_geometry_call(material->shadow.shgrp, ob, geom);

    is_shadow_caster = is_shadow_caster || material->shadow.shgrp != nullptr;
    is_alpha_blend = is_alpha_blend || material->is_alpha_blend_transparent;
  }

  // shadows.sync_object(ob, ob_handle, is_shadow_caster, is_alpha_blend);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPencil
 * \{ */

#define DO_BATCHING true

struct gpIterData {
  Instance &inst;
  Object *ob;
  MaterialArray &material_array;
  int cfra;

  /* Drawcall batching. */
  GPUBatch *geom = nullptr;
  Material *material = nullptr;
  int vfirst = 0;
  int vcount = 0;
  bool instancing = false;

  gpIterData(Instance &inst_, Object *ob_, ObjectHandle &ob_handle)
      : inst(inst_),
        ob(ob_),
        material_array(inst_.materials.material_array_get(
            ob_, inst_.velocity.step_object_sync(ob, ob_handle.object_key, ob_handle.recalc)))
  {
    cfra = DEG_get_ctime(inst.depsgraph);
  };
};

static void gpencil_drawcall_flush(gpIterData &iter)
{
  if (iter.geom != nullptr) {
    shgroup_geometry_call(iter.material->shading.shgrp,
                          iter.ob,
                          iter.geom,
                          iter.vfirst,
                          iter.vcount,
                          iter.instancing);
    shgroup_geometry_call(iter.material->prepass.shgrp,
                          iter.ob,
                          iter.geom,
                          iter.vfirst,
                          iter.vcount,
                          iter.instancing);
    shgroup_geometry_call(iter.material->shadow.shgrp,
                          iter.ob,
                          iter.geom,
                          iter.vfirst,
                          iter.vcount,
                          iter.instancing);
  }
  iter.geom = nullptr;
  iter.vfirst = -1;
  iter.vcount = 0;
}

/* Group draw-calls that are consecutive and with the same type. Reduces GPU driver overhead. */
static void gpencil_drawcall_add(gpIterData &iter,
                                 GPUBatch *geom,
                                 Material *material,
                                 int v_first,
                                 int v_count,
                                 bool instancing)
{
  int last = iter.vfirst + iter.vcount;
  /* Interrupt draw-call grouping if the sequence is not consecutive. */
  if (!DO_BATCHING || (geom != iter.geom) || (material != iter.material) || (v_first - last > 3)) {
    gpencil_drawcall_flush(iter);
  }
  iter.geom = geom;
  iter.material = material;
  iter.instancing = instancing;
  if (iter.vfirst == -1) {
    iter.vfirst = v_first;
  }
  iter.vcount = v_first + v_count - iter.vfirst;
}

static void gpencil_stroke_sync(bGPDlayer *UNUSED(gpl),
                                bGPDframe *UNUSED(gpf),
                                bGPDstroke *gps,
                                void *thunk)
{
  gpIterData &iter = *(gpIterData *)thunk;

  Material *material = iter.material_array.materials[gps->mat_nr];
  MaterialGPencilStyle *gp_style = BKE_gpencil_material_settings(iter.ob, gps->mat_nr + 1);

  bool hide_material = (gp_style->flag & GP_MATERIAL_HIDE) != 0;
  bool show_stroke = ((gp_style->flag & GP_MATERIAL_STROKE_SHOW) != 0) ||
                     (!DRW_state_is_image_render() && ((gps->flag & GP_STROKE_NOFILL) != 0));
  bool show_fill = (gps->tot_triangles > 0) && ((gp_style->flag & GP_MATERIAL_FILL_SHOW) != 0);

  if (hide_material) {
    return;
  }

  if (show_fill) {
    GPUBatch *geom = DRW_cache_gpencil_fills_get(iter.ob, iter.cfra);
    int vfirst = gps->runtime.fill_start * 3;
    int vcount = gps->tot_triangles * 3;
    gpencil_drawcall_add(iter, geom, material, vfirst, vcount, false);
  }

  if (show_stroke) {
    GPUBatch *geom = DRW_cache_gpencil_strokes_get(iter.ob, iter.cfra);
    /* Start one vert before to have gl_InstanceID > 0 (see shader). */
    int vfirst = gps->runtime.stroke_start - 1;
    /* Include "potential" cyclic vertex and start adj vertex (see shader). */
    int vcount = gps->totpoints + 1 + 1;
    gpencil_drawcall_add(iter, geom, material, vfirst, vcount, true);
  }
}

void SyncModule::sync_gpencil(Object *ob, ObjectHandle &ob_handle)
{
  /* TODO(fclem): Waiting for a user option to use the render engine instead of gpencil engine. */
  if (true) {
    inst_.gpencil_engine_enabled = true;
    return;
  }

  gpIterData iter(inst_, ob, ob_handle);

  BKE_gpencil_visible_stroke_iter((bGPdata *)ob->data, nullptr, gpencil_stroke_sync, &iter);

  gpencil_drawcall_flush(iter);

  // bool is_caster = true;      /* TODO material.shadow.shgrp. */
  // bool is_alpha_blend = true; /* TODO material.is_alpha_blend. */
  // shadows.sync_object(ob, ob_handle, is_caster, is_alpha_blend);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Hair
 * \{ */

static void shgroup_curves_call(MaterialPass &matpass,
                                Object *ob,
                                ParticleSystem *part_sys = nullptr,
                                ModifierData *modifier_data = nullptr)
{
  if (matpass.shgrp == nullptr) {
    return;
  }
  if (part_sys != nullptr) {
    DRW_shgroup_hair_create_sub(ob, part_sys, modifier_data, matpass.shgrp, matpass.gpumat);
  }
  else {
    DRW_shgroup_curves_create_sub(ob, matpass.shgrp, matpass.gpumat);
  }
}

void SyncModule::sync_curves(Object *ob, ObjectHandle &ob_handle, ModifierData *modifier_data)
{
  int mat_nr = CURVES_MATERIAL_NR;

  ParticleSystem *part_sys = nullptr;
  if (modifier_data != nullptr) {
    part_sys = reinterpret_cast<ParticleSystemModifierData *>(modifier_data)->psys;
    if (!DRW_object_is_visible_psys_in_active_context(ob, part_sys)) {
      return;
    }
    ParticleSettings *part_settings = part_sys->part;
    const int draw_as = (part_settings->draw_as == PART_DRAW_REND) ? part_settings->ren_as :
                                                                     part_settings->draw_as;
    if (draw_as != PART_DRAW_PATH) {
      return;
    }
    mat_nr = part_settings->omat;
  }

  bool has_motion = inst_.velocity.step_object_sync(ob, ob_handle.object_key, ob_handle.recalc);
  Material &material = inst_.materials.material_get(ob, has_motion, mat_nr - 1, MAT_GEOM_CURVES);

  shgroup_curves_call(material.shading, ob, part_sys, modifier_data);
  shgroup_curves_call(material.prepass, ob, part_sys, modifier_data);
  shgroup_curves_call(material.shadow, ob, part_sys, modifier_data);

  /* TODO(fclem) Hair velocity. */
  // shading_passes.velocity.gpencil_add(ob, ob_handle);

  // bool is_caster = material.shadow.shgrp != nullptr;
  // bool is_alpha_blend = material.is_alpha_blend_transparent;
  // shadows.sync_object(ob, ob_handle, is_caster, is_alpha_blend);
}

/** \} */

}  // namespace blender::eevee
