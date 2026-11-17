/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Scene traversal for eevee_game.
 *
 * This is the module that was missing and causing culling/lights to be empty every frame.
 * It iterates the Depsgraph objects and routes each one to the appropriate sub-module.
 *
 * Adapted from blender::eevee::SyncModule (eevee_sync.cc).
 */

#include "BKE_light.h"
#include "BKE_object.hh"
#include "DEG_depsgraph_query.hh"
#include "DNA_light_types.h"
#include "DNA_object_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_curves_types.h"
#include "DNA_modifier_types.h"
#include "DNA_particle_types.h"
#include "DNA_pointcloud_types.h"

#include "DRW_render.hh"
#include "draw_cache.hh"
#include "draw_common.hh"   /* volume_sub_pass() */

#include "GPU_material.hh"

#include "eevee_game_material.hh"

#include "eevee_game_instance.hh"
#include "eevee_game_sync.hh"

namespace blender::eevee_game {

/* -------------------------------------------------------------------- */
/** \name Handle management
 * \{ */

ObjectHandle &SyncModule::sync_object(const ObjectRef &ob_ref)
{
  ObjectKey key(ob_ref);

  ObjectHandle &handle = ob_handles_.lookup_or_add_cb(key, [&]() {
    ObjectHandle h;
    h.object_key = key;
    return h;
  });

  /* Pull recalc flags directly from the evaluated ID.
   * Mirrors blender::eevee::Instance::get_recalc_flags(): the Depsgraph sets
   * ID_RECALC_TRANSFORM, ID_RECALC_GEOMETRY, etc. on ob_ref.object->id.recalc. */
  handle.recalc = uint(ob_ref.object->id.recalc);
  return handle;
}

void SyncModule::begin_sync()
{
  /* Reset per-frame state on all handles so stale objects are detectable in end_sync(). */
  for (ObjectHandle &h : ob_handles_.values()) {
    h.recalc = 0;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Per-object routing
 * \{ */

void SyncModule::object_sync(const ObjectRef &ob_ref)
{
  Object *ob = ob_ref.object;

  /* Skip objects that are not renderable or not visible in this context. */
  if (!DRW_object_is_renderable(ob)) {
    return;
  }

  const int visibility = DRW_object_visibility_in_active_context(ob);
  if (!(visibility & OB_VISIBLE_SELF)) {
    return;
  }

  ObjectHandle &handle = sync_object(ob_ref);

  switch (ob->type) {
    case OB_MESH:
    case OB_SURF:
    case OB_FONT:
    case OB_MBALL:
      sync_mesh(ob, handle, ob_ref);
      break;

    case OB_CURVES:
      sync_curves(ob, handle, ob_ref);
      break;

    case OB_POINTCLOUD:
      sync_pointcloud(ob, handle, ob_ref);
      break;

    case OB_LAMP:
      sync_light(ob, handle);
      break;

    /* Volume objects need their density voxelized into the froxel grid, and
     * the fog is still a homogeneous medium. */
    default:
      break;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mesh sync
 * \{ */

void SyncModule::sync_mesh(Object *ob, ObjectHandle &handle, const ObjectRef &ob_ref)
{
  /* Assign a stable GPU resource handle. This index is shared by:
   *   - The Manager bounds buffer read by GameView for culling
   *   - The velocity indirection buffer in VelocityModule
   *   - The per-draw resource index used by DRW for object UBO binding */
  ResourceHandleRange res_handle = inst_.manager->unique_handle(ob_ref);

  /* Velocity must be synced before material_array_get() because has_motion
   * drives the prepass pipeline choice (with/without velocity output).
   * Only FSR reads motion vectors. Without it no object is tracked: no matrix
   * compare, no vertex copy of deformed meshes, no velocity variant of the
   * depth shader. The velocity map then empties itself in end_sync(), so
   * enabling FSR starts from zero motion. */
  const bool has_motion = inst_.upscale.is_initialized() &&
                          inst_.velocity.step_object_sync(
                              handle.object_key, ob_ref, handle.recalc, res_handle);

  /* Compile or retrieve GPUMaterials for all slots and assign them to the
   * correct pipeline sub-passes (prepass, gbuffer, shadow, forward). */
  MaterialArray &mat_array = inst_.materials.material_array_get(ob, has_motion);

  /* Retrieve the per-slot GPU batches that match the compiled GPUMaterials.
   * DRW_cache_object_surface_material_get() may return nullptrs for empty slots. */
  Span<gpu::Batch *> batches =
      DRW_cache_object_surface_material_get(ob, mat_array.gpu_materials);

  if (batches.is_empty()) {
    return;
  }

  bool has_volume          = false;
  float inflate_bounds     = 0.0f;

  for (auto i : mat_array.materials.index_range()) {
    gpu::Batch *geom = batches[i];
    if (!geom) {
      continue;
    }

    Material &mat           = mat_array.materials[i];
    GPUMaterial *gpu_mat    = mat_array.gpu_materials[i];

    /* Constant medium filling a box-shaped mesh: evaluated analytically by
     * the volume scatter pass, no voxelization. The cached Material is shared
     * by other objects, so the choice stays local. */
    bool use_volume_passes = mat.has_volume;
    if (mat.has_volume && ob->type == OB_MESH && !(ob->visibility_flag & OB_HIDE_CAMERA)) {
      const blender::Material *bmat = GPU_material_get_material(gpu_mat);
      const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(*ob);
      if (bmat != nullptr && inst_.volume.box_add(ob, mesh, bmat->nodetree)) {
        if (!mat.has_surface) {
          continue;
        }
        use_volume_passes = false;
      }
    }

    /* Volume embedded in a mesh material (e.g. glass with participating media) */
    if (use_volume_passes) {
      if (mat.volume_occupancy.sub_pass) {
        /* volume_sub_pass() sets up per-object voxelisation state */
        PassMain::Sub *vol_pass =
            volume_sub_pass(*mat.volume_occupancy.sub_pass, inst_.scene, ob,
                            mat.volume_occupancy.gpumat);
        if (vol_pass) {
          vol_pass->draw(geom, res_handle);
        }
      }
      if (mat.volume_material.sub_pass) {
        PassMain::Sub *vol_pass =
            volume_sub_pass(*mat.volume_material.sub_pass, inst_.scene, ob,
                            mat.volume_material.gpumat);
        if (vol_pass) {
          vol_pass->draw(geom, res_handle);
        }
      }
      has_volume = true;
      if (!mat.has_surface) {
        continue; /* Volume-only material: no surface draw call */
      }
    }

    /* Depth/velocity prepass */
    if (mat.prepass.sub_pass) {
      mat.prepass.sub_pass->draw(geom, res_handle);
    }
    /* G-Buffer fill or forward shading */
    if (mat.shading.sub_pass) {
      mat.shading.sub_pass->draw(geom, res_handle);
    }
    /* Shadow map rendering */
    if (mat.shadow.sub_pass) {
      mat.shadow.sub_pass->draw(geom, res_handle);
    }
    /* Transparent forward overlap masking */
    if (mat.overlap_masking.sub_pass) {
      mat.overlap_masking.sub_pass->draw(geom, res_handle);
    }

    /* Displacement inflate: expand bounding volume to cover tessellation */
    if (GPU_material_has_displacement_output(gpu_mat)) {
      blender::Material *bmat = GPU_material_get_material(gpu_mat);
      if (bmat) {
        inflate_bounds = math::max(inflate_bounds, bmat->inflate_bounds);
      }
    }
  }

  /* Expand bounding box so culling is conservative with vertex displacement */
  if (inflate_bounds != 0.0f) {
    inst_.manager->update_handle_bounds(res_handle, ob_ref, inflate_bounds);
  }

  /* Volume object sync (registers the object into the froxel grid pipeline) */
  if (has_volume) {
    inst_.volume.object_sync(handle);
  }

  /* Shadow caster registration */
  inst_.shadows.sync_object(ob, handle.object_key.hash());

  /* Per-draw attributes (e.g. UV maps, vertex colors used by materials) */
  inst_.manager->extract_object_attributes(res_handle, ob_ref, mat_array.gpu_materials);

  sync_hair_particles(ob, ob_ref, res_handle);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Curves, point clouds, particle hair
 *
 * Same passes as a mesh, but the geometry is expanded in the vertex shader
 * from buffer textures the draw module binds per sub-pass, so every pass of
 * the material gets its own small sub-pass. No velocity (FSR sees the camera
 * motion only) and no volume: neither has a pipeline here yet.
 * \{ */

template<typename SetupFn>
static void surface_drawcalls(Material &mat, ResourceHandleRange res_handle, SetupFn setup)
{
  if (mat.has_volume && !mat.has_surface) {
    return;
  }
  for (MaterialPass *matpass : {&mat.prepass, &mat.shading, &mat.shadow, &mat.overlap_masking}) {
    if (matpass->sub_pass == nullptr) {
      continue;
    }
    PassMain::Sub &sub = matpass->sub_pass->sub("Geometry");
    gpu::Batch *geom = setup(sub, matpass->gpumat);
    sub.draw(geom, res_handle);
  }
}

void SyncModule::sync_curves(Object *ob, ObjectHandle &handle, const ObjectRef &ob_ref)
{
  ResourceHandleRange res_handle = inst_.manager->unique_handle(ob_ref);
  Material &mat = inst_.materials.material_get(
      ob, false, CURVES_MATERIAL_NR - 1, eevee::MAT_GEOM_CURVES);
  surface_drawcalls(mat, res_handle, [&](PassMain::Sub &sub, GPUMaterial *gpumat) {
    const char *error = nullptr;
    return curves_sub_pass_setup(sub, inst_.scene, ob, error, gpumat);
  });
  inst_.shadows.sync_object(ob, handle.object_key.hash());
  inst_.manager->extract_object_attributes(res_handle, ob_ref, mat.shading.gpumat);
}

void SyncModule::sync_pointcloud(Object *ob, ObjectHandle &handle, const ObjectRef &ob_ref)
{
  ResourceHandleRange res_handle = inst_.manager->unique_handle(ob_ref);
  Material &mat = inst_.materials.material_get(
      ob, false, POINTCLOUD_MATERIAL_NR - 1, eevee::MAT_GEOM_POINTCLOUD);
  surface_drawcalls(mat, res_handle, [&](PassMain::Sub &sub, GPUMaterial *gpumat) {
    gpu::Batch *geom = pointcloud_sub_pass_setup(sub, ob, gpumat);
    /* Front-facing spheres only: back faces are for volumes. */
    sub.push_constant("ptcloud_backface", false);
    return geom;
  });
  inst_.shadows.sync_object(ob, handle.object_key.hash());
  inst_.manager->extract_object_attributes(res_handle, ob_ref, mat.shading.gpumat);
}

void SyncModule::sync_hair_particles(Object *ob,
                                     const ObjectRef &ob_ref,
                                     ResourceHandleRange res_handle)
{
  for (ModifierData &md : ob->modifiers) {
    if (md.type != eModifierType_ParticleSystem) {
      continue;
    }
    ParticleSystem *psys = reinterpret_cast<ParticleSystemModifierData *>(&md)->psys;
    const ParticleSettings *part = psys->part;
    /* What the viewport shows. */
    const int draw_as = (part->draw_as == PART_DRAW_REND) ? part->ren_as : part->draw_as;
    if (draw_as != PART_DRAW_PATH || !DRW_object_is_visible_psys_in_active_context(ob, psys)) {
      continue;
    }
    Material &mat = inst_.materials.material_get(
        ob, false, part->omat - 1, eevee::MAT_GEOM_CURVES);
    surface_drawcalls(mat, res_handle, [&](PassMain::Sub &sub, GPUMaterial *gpumat) {
      return hair_sub_pass_setup(sub, inst_.scene, ob_ref, psys, &md, gpumat);
    });
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Light sync
 * \{ */

void SyncModule::sync_light(Object *ob, ObjectHandle &handle)
{
  const blender::Light *la = &DRW_object_get_data_for_drawing<const blender::Light>(*ob);

  /* Build a LightEntry from the Blender light DNA and the evaluated object matrix. */
  LightEntry entry;
  entry.object_to_world = ob->object_to_world();
  entry.uid = handle.object_key.hash();

  /* Map Blender light type to our LightType enum. */
  switch (la->type) {
    case LA_SUN:
      entry.data.type = uint32_t(LightType::SUN);
      break;
    case LA_LOCAL:
      entry.data.type = uint32_t(LightType::PUNCTUAL_POINT);
      break;
    case LA_SPOT:
      entry.data.type = uint32_t(LightType::PUNCTUAL_SPOT);
      entry.data.spot_angle = la->spotsize;
      break;
    case LA_AREA:
      entry.data.type = uint32_t(LightType::AREA_RECT);
      break;
    default:
      entry.data.type = uint32_t(LightType::PUNCTUAL_POINT);
      break;
  }

  /* Position from world matrix translation column. */
  entry.data.position  = float3(entry.object_to_world[3]);
  /* Lights shine down their local -Z axis. Normalized here so the shaders
   * do not have to, and so object scale cannot leak into the lighting. */
  entry.data.direction = -math::normalize(float3(entry.object_to_world[2]));
  /* Temperature and exposure included, like EEVEE. */
  entry.data.color     = BKE_light_color(*la);
  entry.data.energy    = BKE_light_power(*la);
  /* Emitter size: soft-shadow radius for punctual lights, panel size for area lights. */
  entry.data.radius = (la->type == LA_AREA) ? la->area_size : la->radius;
  /* Influence radius: the custom distance, else where the inverse square
   * falloff of the brightest channel drops under the scene light threshold.
   * Same rule as EEVEE Light::attenuation_radius_get() and the gameengine
   * KX_CullingGate, so a light culled on CPU is one that contributes nothing. */
  if (la->mode & LA_CUSTOM_ATTENUATION) {
    entry.data.attenuation = la->att_dist;
  }
  else {
    const float threshold = max_ff(1e-16f, inst_.scene->eevee.light_threshold);
    const float max_power = math::reduce_max(entry.data.color) * fabsf(entry.data.energy / 100.0f);
    const float power = max_fff(la->diff_fac, la->spec_fac, la->volume_fac) * max_power;
    entry.data.attenuation = sqrtf(power / threshold);
  }

  /* Shadow: -1 means no shadow; ShadowModule::end_sync() will assign atlas slots. */
  entry.data.shadow_index = (la->mode & LA_SHADOW) ? 0 : -1;

  inst_.lights.add(entry);
}

/** \} */

} // namespace blender::eevee_game
