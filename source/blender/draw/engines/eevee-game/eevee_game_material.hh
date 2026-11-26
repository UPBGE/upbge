/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DNA_material_types.h"
#include "DRW_render.hh"
#include "BLI_map.hh"
#include "BLI_vector.hh"
#include "GPU_material.hh"
#include "draw_pass.hh"

/* Reuse EEVEE's material shared types: eMaterialPipeline, eMaterialGeometry, etc.
 * These enums map directly to the GLSL shader permutations we reuse from EEVEE. */
#include "../eevee/eevee_material_shared.hh"

namespace blender::eevee_game {

using namespace draw;

class GameInstance;

/* -------------------------------------------------------------------- */
/** \name Material pipeline selection helpers (adapted from eevee_material.hh)
 * \{ */

static inline eevee::eMaterialGeometry to_material_geometry(const Object *ob)
{
  using namespace eevee;
  switch (ob->type) {
    case OB_CURVES:     return MAT_GEOM_CURVES;
    case OB_VOLUME:     return MAT_GEOM_VOLUME;
    case OB_POINTCLOUD: return MAT_GEOM_POINTCLOUD;
    default:            return MAT_GEOM_MESH;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name MaterialPass / Material / MaterialArray
 *
 * Identical layout to blender::eevee versions — sync.cc code uses the same fields.
 * We only keep the passes relevant to eevee_game (no probe/capture/bake passes).
 * \{ */

struct MaterialPass {
  GPUMaterial    *gpumat   = nullptr;
  PassMain::Sub  *sub_pass = nullptr;
};

struct Material {
  bool is_alpha_blend_transparent = false;
  bool has_transparent_shadows    = false;
  bool has_surface                = false;
  bool has_volume                 = false;

  /* Depth/velocity prepass */
  MaterialPass prepass;
  /* Main surface shading (deferred G-Buffer fill or forward) */
  MaterialPass shading;
  /* Shadow map rendering */
  MaterialPass shadow;
  /* Transparent overlap masking (forward only) */
  MaterialPass overlap_masking;
  /* Volume occupancy and material (objects with volume shader) */
  MaterialPass volume_occupancy;
  MaterialPass volume_material;
};

struct MaterialArray {
  Vector<Material>      materials;
  Vector<GPUMaterial *> gpu_materials;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name MaterialModule
 *
 * Manages GPUMaterial compilation, sub-pass assignment, and texture loading.
 * Adapted from blender::eevee::MaterialModule with the following changes:
 *   - No probe/capture/baking pipeline passes.
 *   - No reference to eevee::Instance — uses GameInstance instead.
 *   - material_pass_get() delegates shader creation to eevee_game ShaderModule
 *     (which in turn calls the same GPU_material / EEVEE shader backend).
 *   - Volume layers are handled directly (no separate VolumePipeline).
 * \{ */

class MaterialModule {
 public:
  /* Default materials created at startup — used as fallbacks during async compilation. */
  blender::Material *diffuse_mat     = nullptr;
  blender::Material *metallic_mat    = nullptr;
  blender::Material *default_surface = nullptr;
  blender::Material *default_volume  = nullptr;

  /* Optional scene-level material override (from ViewLayer). */
  blender::Material *material_override = nullptr;

  /* Async compilation counters (read by the UI overlay). */
  int64_t queued_shaders_count          = 0;

  /* A shader finished compiling since the previous sync: what was drawn with a fallback
   * material while it compiled is out of date (cached shadow maps). */
  bool shaders_compiled_since_last_sync() const
  {
    return gpu_pass_next_update_ != gpu_pass_last_update_;
  }
  int64_t queued_textures_count         = 0;
  int64_t queued_optimize_shaders_count = 0;

  MaterialModule(GameInstance &inst);
  ~MaterialModule();

  /** Reset per-frame maps. Must be called before any material_array_get() calls. */
  void begin_sync();

  /** Finish texture loading started during material_array_get(). */
  void end_sync();

  /**
   * Returns a MaterialArray describing every material slot of ob for the current frame.
   * References are valid until the next call to material_array_get() or material_get().
   */
  MaterialArray &material_array_get(Object *ob, bool has_motion);

  /**
   * Returns the Material for a specific slot and geometry type.
   * Used by curves, volumes, etc.
   */
  Material &material_get(Object *ob,
                         bool has_motion,
                         int mat_nr,
                         eevee::eMaterialGeometry geometry_type);

 private:
  /** Compile or retrieve a GPUMaterial and assign it to a sub-pass. */
  MaterialPass material_pass_get(Object *ob,
                                 blender::Material *blender_mat,
                                 eevee::eMaterialPipeline pipeline_type,
                                 eevee::eMaterialGeometry geometry_type,
                                 bool shadow_caster = false);

  /** Create/update a Material entry in material_map_ for this object+material combination. */
  Material &material_sync(Object *ob,
                           blender::Material *blender_mat,
                           eevee::eMaterialGeometry geometry_type,
                           bool has_motion);

  /** Return the evaluated material from slot, or the default surface/volume material. */
  blender::Material *material_from_slot(Object *ob, int slot);

  /** Queue image textures used by this GPUMaterial for background loading. */
  void queue_texture_loading(GPUMaterial *material);

  GameInstance &inst_;

  /* Per-frame material and shader caches. Cleared in begin_sync(). */
  Map<uint64_t, Material>           material_map_;
  Map<uint64_t, PassMain::Sub *>    shader_map_;
  /* Same, for the shadow-only casters (PipelineModule::shadow_caster_add). */
  Map<uint64_t, PassMain::Sub *>    shadow_shader_map_;

  MaterialArray material_array_;

  blender::Material *error_mat_ = nullptr;

  /* Timestamp of last GPU pass update — used to detect newly compiled shaders. */
  uint64_t gpu_pass_last_update_ = 0;
  uint64_t gpu_pass_next_update_ = 0;

  /* Textures that need CPU-side loading and GPU upload this frame. */
  Vector<GPUMaterialTexture *> texture_loading_queue_;
};

/** \} */

} // namespace blender::eevee_game
