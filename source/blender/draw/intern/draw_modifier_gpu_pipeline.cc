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
#include "draw_lattice_deform.hh"
#include "draw_shapekeys_skinning.hh"

#include "BKE_mesh_gpu.hh"

namespace blender::draw {

int GPUModifierPipeline::instance_counter = 0;

GPUModifierPipeline::GPUModifierPipeline() : instance_id(++instance_counter) {}

GPUModifierPipeline::~GPUModifierPipeline() {}

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
         * Basis change, Relative To, Edit Mode changes, etc.)
         *
         * Note:
         * mesh_orig_ is always set by execute() before calling compute_fast_hash(),
         * so
         * the else branch should never be reached. We assert this invariant. */
        if (mesh_orig_) {
          hash = BLI_hash_int_2d(hash, ShapeKeySkinningManager::compute_shapekey_hash(mesh_orig_));
        }
        else {
          /* Defensive fallback: Should never happen in normal execution.
           * If
           * mesh_orig_ is not set, it means compute_fast_hash() was called
           * outside of
           * execute(), which is a programming error. */
          BLI_assert_unreachable();

          /* Hash basic Key properties as emergency fallback */
          Key *key = static_cast<Key *>(stage.modifier_data);
          hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(key)));
        }
        break;
      }
      case ModifierGPUStageType::ARMATURE: {
        /* Armature: Delegate to ArmatureSkinningManager for complete hash
         * (detects armature change, DQS mode, vertex groups, bone count, etc.) */
        if (mesh_orig_) {
          ArmatureModifierData *amd = static_cast<ArmatureModifierData *>(stage.modifier_data);
          hash = BLI_hash_int_2d(
              hash, ArmatureSkinningManager::compute_armature_hash(mesh_orig_, amd));
        }
        else {
          /* Defensive fallback: Should never happen in normal execution */
          BLI_assert_unreachable();

          /* Hash basic modifier properties as emergency fallback */
          ModifierData *md = static_cast<ModifierData *>(stage.modifier_data);
          hash = BLI_hash_int_2d(hash, uint32_t(md->persistent_uid));
        }
        break;
      }
      case ModifierGPUStageType::LATTICE: {
        /* Lattice: Delegate to LatticeSkinningManager for complete hash
         * (detects lattice change, dimensions, interpolation types, vertex groups, etc.) */
        if (mesh_orig_) {
          LatticeModifierData *lmd = static_cast<LatticeModifierData *>(stage.modifier_data);
          hash = BLI_hash_int_2d(
              hash, LatticeSkinningManager::compute_lattice_hash(mesh_orig_, lmd));
        }
        else {
          /* Defensive fallback: Should never happen in normal execution */
          BLI_assert_unreachable();

          /* Hash basic modifier properties as emergency fallback */
          ModifierData *md = static_cast<ModifierData *>(stage.modifier_data);
          hash = BLI_hash_int_2d(hash, uint32_t(md->persistent_uid));
        }
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
    case ModifierGPUStageType::LATTICE:
      LatticeSkinningManager::instance().invalidate_all(mesh_owner);
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

  /* Store references for hash computation */
  mesh_orig_ = mesh_owner;
  ob_eval_ = ob;

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
  gpu::StorageBuf *current_buffer = buffer_a_;

  for (int stage_idx : stages_.index_range()) {
    const ModifierGPUStage &stage = stages_[stage_idx];

    /* Dispatch stage: manager reads from current_buffer and returns its output buffer.
     * Pass pipeline_hash_ to allow manager to detect changes without recomputing hash. */
    gpu::StorageBuf *result = stage.dispatch_fn(
        mesh, ob, stage.modifier_data, current_buffer, nullptr, pipeline_hash_);

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
                                                 gpu::StorageBuf *output,
                                                 uint32_t pipeline_hash)
{
  /* ShapeKeys are always first, so they don't need input buffer.
   * They compute: output = rest
   * + sum(delta_k * weight_k) */

  Mesh *mesh_eval = static_cast<Mesh *>(ob_eval->data);
  MeshBatchCache *cache = static_cast<MeshBatchCache *>(mesh_eval->runtime->batch_cache);
  if (!cache) {
    return nullptr;
  }

  /* Call existing ShapeKey manager, passing the pipeline hash */
  ShapeKeySkinningManager &sk_mgr = ShapeKeySkinningManager::instance();
  sk_mgr.ensure_static_resources(mesh_orig, pipeline_hash);

  return sk_mgr.dispatch_shapekeys(cache, ob_eval, output);
}

static gpu::StorageBuf *dispatch_armature_stage(Mesh *mesh_orig,
                                                Object *ob_eval,
                                                void *modifier_data,
                                                gpu::StorageBuf *input,
                                                gpu::StorageBuf * /*output*/,
                                                uint32_t pipeline_hash)
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

  ArmatureSkinningManager &arm_mgr = ArmatureSkinningManager::instance();

  /* IMPORTANT: amd comes from ORIGINAL object (build_gpu_modifier_pipeline uses orig_ob),
   * so
   * amd->object is the ORIGINAL armature. We just need to get the evaluated version. */
  Object *orig_arma = amd->object;
  Object *eval_arma = static_cast<Object *>(
      DEG_get_evaluated(DRW_context_get()->depsgraph, orig_arma));

  /* Pass amd (original) for settings extraction */
  arm_mgr.ensure_static_resources(amd, orig_arma, ob_eval, mesh_orig, pipeline_hash);

  return arm_mgr.dispatch_skinning(
      amd, DRW_context_get()->depsgraph, eval_arma, ob_eval, cache, input);
}

static gpu::StorageBuf *dispatch_lattice_stage(Mesh *mesh_orig,
                                               Object *ob_eval,
                                               void *modifier_data,
                                               gpu::StorageBuf *input,
                                               gpu::StorageBuf * /*output*/,
                                               uint32_t pipeline_hash)
{
  LatticeModifierData *lmd = static_cast<LatticeModifierData *>(modifier_data);
  if (!lmd || !lmd->object) {
    return nullptr;
  }

  Mesh *mesh_eval = static_cast<Mesh *>(ob_eval->data);
  MeshBatchCache *cache = static_cast<MeshBatchCache *>(mesh_eval->runtime->batch_cache);
  if (!cache) {
    return nullptr;
  }

  LatticeSkinningManager &lat_mgr = LatticeSkinningManager::instance();

  /* IMPORTANT: lmd comes from ORIGINAL object (build_gpu_modifier_pipeline uses orig_ob),
   * so
   * lmd->object is the ORIGINAL lattice. We just need to get the evaluated version. */
  Object *orig_lattice = lmd->object;
  Object *eval_lattice = static_cast<Object *>(
      DEG_get_evaluated(DRW_context_get()->depsgraph, orig_lattice));

  /* Pass lmd (original) for settings extraction */
  lat_mgr.ensure_static_resources(lmd, orig_lattice, ob_eval, mesh_orig, pipeline_hash);

  return lat_mgr.dispatch_deform(
      lmd, DRW_context_get()->depsgraph, eval_lattice, ob_eval, cache, input);
}

/** \} */

bool build_gpu_modifier_pipeline(Object &ob_eval, Mesh &mesh_orig, GPUModifierPipeline &pipeline)
{
  /* Don't clear the pipeline here! Let execute() handle hash-based invalidation.
   * This
   * preserves pipeline_hash_ across frames for stable change detection. */
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
   * IMPORTANT: Use ORIGINAL object modifiers, not evaluated
   * ones!
   * This ensures modifier data pointers match what BKE_modifiers_is_deformed_by_*
   * expects.
   * The evaluated object is passed separately to dispatch functions for runtime
   * data. */
  Object *orig_ob = DEG_get_original(&ob_eval);
  for (ModifierData *md = static_cast<ModifierData *>(orig_ob->modifiers.first); md; md = md->next)
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
      case eModifierType_Lattice: {
        pipeline.add_stage(
            ModifierGPUStageType::LATTICE, md, execution_order++, dispatch_lattice_stage);
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
