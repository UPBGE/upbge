/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_modifier_gpu_pipeline.hh"

#include <algorithm>

#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BLI_hash.h"
#include "DEG_depsgraph_query.hh"
#include "DNA_armature_types.h"
#include "DNA_key_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

#include "DRW_render.hh"

#include "draw_armature_skinning.hh"
#include "draw_cache_impl.hh"
#include "draw_shapekeys_skinning.hh"

#include "BKE_mesh_gpu.hh"

namespace blender::draw {

GPUModifierPipeline::~GPUModifierPipeline()
{
  /* Buffers are now managed by mesh_gpu_cache, no manual cleanup needed */
}

void GPUModifierPipeline::add_stage(ModifierGPUStageType type,
                                    void *modifier_data,
                                    int execution_order,
                                    ModifierGPUStage::DispatchFunc dispatch_fn)
{
  ModifierGPUStage stage;
  stage.type = type;
  stage.modifier_data = modifier_data;
  stage.execution_order = execution_order;
  stage.dispatch_fn = dispatch_fn;

  stages_.append(stage);
  needs_recompile_ = true;
}

void GPUModifierPipeline::sort_stages()
{
  std::sort(
      stages_.begin(), stages_.end(), [](const ModifierGPUStage &a, const ModifierGPUStage &b) {
        return a.execution_order < b.execution_order;
      });
}

void GPUModifierPipeline::allocate_buffers(Mesh *mesh_owner, int vertex_count)
{
  /* Use stable key attached to the original mesh (mesh_owner) */
  const std::string key_buffer_a = "gpu_pipeline_buffer_a";

  /* Try to get existing buffer from mesh GPU cache */
  buffer_a_ = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_buffer_a);

  /* Allocate if not present */
  const size_t buffer_size = size_t(vertex_count) * sizeof(float) * 4;

  if (!buffer_a_) {
    buffer_a_ = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_buffer_a, buffer_size);
  }
}

uint32_t GPUModifierPipeline::compute_fast_hash() const
{
  uint32_t hash = 0;

  for (const ModifierGPUStage &stage : stages_) {
    /* Hash execution order (detects reordering) */
    hash = BLI_hash_int_2d(hash, uint32_t(stage.execution_order));

    switch (stage.type) {
      case ModifierGPUStageType::SHAPEKEYS: {
        /* ShapeKeys: Delegate to ShapeKeySkinningManager for complete hash
         * (detects
         * Basis change, Relative To, Edit Mode changes, etc.) */

        /* Note: We need the Mesh* to compute the hash, but stage.modifier_data is Key*.
         *
         * The Key* is always attached to a Mesh, but we don't have direct access here.
         *
         * As a workaround, we still hash the Key* pointer + basic properties,
         * and rely
         * on ShapeKeySkinningManager::ensure_static_resources() to detect
         * detailed
         * changes and invalidate GPU resources when needed. */

        Key *key = static_cast<Key *>(stage.modifier_data);
        hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(key)));
        hash = BLI_hash_int_2d(hash, uint32_t(key->deform_method));
        hash = BLI_hash_int_2d(hash, uint32_t(key->totkey));
        hash = BLI_hash_int_2d(hash, uint32_t(key->type));

        /* TODO: To fully detect ShapeKey changes in the pipeline hash,
         * we would need
         * access to the Mesh* here to call:
         * hash = BLI_hash_int_2d(hash,
         * ShapeKeySkinningManager::compute_shapekey_hash(mesh));
         *
         * For now,
         * ShapeKeySkinningManager handles invalidation internally
         * when
         * ensure_static_resources() detects changes. */
        break;
      }
      case ModifierGPUStageType::ARMATURE: {
        /* Modifiers: Hash persistent_uid + type + mode */
        ModifierData *md = static_cast<ModifierData *>(stage.modifier_data);
        hash = BLI_hash_int_2d(hash, uint32_t(md->persistent_uid));
        hash = BLI_hash_int_2d(hash, uint32_t(md->type));
        hash = BLI_hash_int_2d(hash, uint32_t(md->mode));
        break;
      }
      default:
        /* Unsupported type: just hash the pointer */
        hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(stage.modifier_data)));
        break;
    }
  }
  return hash;
}

void GPUModifierPipeline::invalidate_stage(ModifierGPUStageType type, Mesh *mesh_owner)
{
  /* Notify the corresponding Manager to free ALL GPU resources (shaders + SSBOs) */
  switch (type) {
    case ModifierGPUStageType::SHAPEKEYS:
      ShapeKeySkinningManager::instance().invalidate_all(mesh_owner);
      break;
    case ModifierGPUStageType::ARMATURE:
      ArmatureSkinningManager::instance().invalidate_all(mesh_owner);
      break;
    default:
      break;
  }
}

gpu::StorageBuf *GPUModifierPipeline::execute(Mesh *mesh, Object *ob, MeshBatchCache *cache)
{
  if (stages_.is_empty()) {
    return nullptr;
  }

  sort_stages();

  /* Get mesh_owner (original mesh) for stable GPU cache keys */
  Mesh *mesh_owner = cache->mesh_owner ? cache->mesh_owner : mesh;
  const int vertex_count = mesh_owner->verts_num;

  /* Allocate buffer (pre-filled with rest positions on first allocation) */
  allocate_buffers(mesh_owner, vertex_count);

  /* Check if pipeline structure changed (order, add/remove, enable/disable) */
  const uint32_t new_hash = compute_fast_hash();
  if (new_hash != pipeline_hash_) {
    pipeline_hash_ = new_hash;

    /* Pipeline changed → Invalidate ALL stages (shaders + SSBOs) */
    for (const ModifierGPUStage &stage : stages_) {
      invalidate_stage(stage.type, mesh_owner);
    }

    needs_recompile_ = true;
  }

  /* Chain stages: output of stage N becomes input of stage N+1 */
  gpu::StorageBuf *current_buffer = buffer_a_;  // Start with rest positions

  for (int stage_idx : stages_.index_range()) {
    const ModifierGPUStage &stage = stages_[stage_idx];

    /* Dispatch stage: manager reads from current_buffer and returns its output buffer */
    gpu::StorageBuf *result = stage.dispatch_fn(
        mesh, ob, stage.modifier_data, current_buffer, nullptr);

    if (!result) {
      /* Stage failed, abort pipeline */
      return nullptr;
    }

    /* Use the result as input for the next stage */
    current_buffer = result;
  }

  needs_recompile_ = false;
  return current_buffer;
}

void GPUModifierPipeline::clear()
{
  stages_.clear();
  /* Buffer is managed by mesh_gpu_cache, just reset pointer */
  buffer_a_ = nullptr;
  /* Note: Do NOT reset pipeline_hash_ here! It's used to detect pipeline changes
   * across frames. Resetting it would cause unnecessary invalidations every frame. */
  needs_recompile_ = false;
}

void GPUModifierPipeline::clear_stages()
{
  /* Clear only the stages list, preserve pipeline_hash_ for change detection */
  stages_.clear();
  /* Don't touch buffer_a_, pipeline_hash_, or needs_recompile_ */
}

void GPUModifierPipeline::invalidate_shaders()
{
  needs_recompile_ = true;
}

/* -------------------------------------------------------------------- */
/** \name Pipeline Construction from Modifier Stack
 * \{ */

/** \name Dispatch Functions (Adapters)
 *
 * These functions adapt the generic pipeline interface to the specific
 * manager APIs (ShapeKeys, Armature, etc.)
 * \{ */

static gpu::StorageBuf *dispatch_shapekeys_stage(Mesh *mesh_orig,
                                                 Object *ob_eval,
                                                 void * /*modifier_data*/,
                                                 gpu::StorageBuf * /*input*/,
                                                 gpu::StorageBuf *output)
{
  /* ShapeKeys are always first, so they don't need input buffer.
   * They compute: output = rest + sum(delta_k * weight_k) */

  Mesh *mesh_eval = static_cast<Mesh *>(ob_eval->data);
  MeshBatchCache *cache = static_cast<MeshBatchCache *>(mesh_eval->runtime->batch_cache);
  if (!cache) {
    return nullptr;
  }

  /* Call existing ShapeKey manager */
  ShapeKeySkinningManager &sk_mgr = ShapeKeySkinningManager::instance();
  sk_mgr.ensure_static_resources(mesh_orig);

  return sk_mgr.dispatch_shapekeys(cache, ob_eval, output);
}

static gpu::StorageBuf *dispatch_armature_stage(Mesh *mesh_orig,
                                                Object *ob_eval,
                                                void *modifier_data,
                                                gpu::StorageBuf *input,
                                                gpu::StorageBuf * /*output*/)
{
  ArmatureModifierData *amd = static_cast<ArmatureModifierData *>(modifier_data);
  if (!amd || !amd->object) {
    return nullptr;
  }

  Mesh *mesh_eval = static_cast<Mesh *>(ob_eval->data);
  MeshBatchCache *cache = static_cast<MeshBatchCache *>(mesh_eval->runtime->batch_cache);
  if (!cache) {
    return nullptr;
  }

  /* Call existing Armature manager with simplified signature */
  ArmatureSkinningManager &arm_mgr = ArmatureSkinningManager::instance();

  Object *orig_arma = BKE_modifiers_is_deformed_by_armature(DEG_get_original(ob_eval));
  Object *eval_arma = static_cast<Object *>(
      DEG_get_evaluated(DRW_context_get()->depsgraph, orig_arma));
  /* amd->object is the original armature (from modifier data) */
  arm_mgr.ensure_static_resources(orig_arma, ob_eval, mesh_orig);

  /* dispatch_skinning now only takes SSBO input/output (no VBO) */
  return arm_mgr.dispatch_skinning(DRW_context_get()->depsgraph,
                                   eval_arma,  // evaluated armature (with animations)
                                   ob_eval,    // deformed_eval
                                   cache,
                                   input);  // ssbo_in (input from previous stage or nullptr)
}

/** \} */

bool build_gpu_modifier_pipeline(Object &ob_eval, Mesh &mesh_orig, GPUModifierPipeline &pipeline)
{
  /* Don't clear the pipeline here! Let execute() handle hash-based invalidation.
   * This preserves pipeline_hash_ across frames for stable change detection. */
  /* Clear stages list to rebuild from scratch (but keep pipeline_hash_ intact) */
  pipeline.clear_stages();

  int execution_order = 0;

  /* 1. ShapeKeys (always first if present) */
  if (mesh_orig.key && (mesh_orig.key->deform_method & KEY_DEFORM_METHOD_GPU)) {
    pipeline.add_stage(ModifierGPUStageType::SHAPEKEYS,
                       mesh_orig.key,
                       execution_order++,
                       dispatch_shapekeys_stage);
  }

  /* 2. Modifiers in stack order
   * NOTE: Pre-filtering is already done in draw_cache_impl_mesh.cc when registering
   * meshes to process (via compute_gpu_playback_decision). We only need to check
   * if the modifier is enabled and dispatch the appropriate stage. */
  for (ModifierData *md = static_cast<ModifierData *>(ob_eval.modifiers.first); md; md = md->next)
  {
    /* Basic validity checks */
    if (!md || !(md->mode & eModifierMode_Realtime)) {
      continue;
    }

    /* Dispatch based on modifier type */
    switch (md->type) {
      case eModifierType_Armature: {
        pipeline.add_stage(
            ModifierGPUStageType::ARMATURE, md, execution_order++, dispatch_armature_stage);
        break;
      }

        /* Add more modifier types here as they are implemented */

      default:
        /* Unsupported modifier type, skip */
        break;
    }
  }

  return pipeline.stage_count() > 0;
}

/** \} */

}  // namespace blender::draw
