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
#include "draw_displace.hh"
#include "draw_hook.hh"
#include "draw_lattice_deform.hh"
#include "draw_shapekeys_skinning.hh"
#include "draw_simpledeform.hh"
#include "draw_wave.hh"

#include "BKE_mesh_gpu.hh"


namespace blender {
namespace draw {

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
}

void GPUModifierPipeline::sort_stages()
{
  std::sort(
      stages_.begin(), stages_.end(), [](const ModifierGPUStage &a, const ModifierGPUStage &b) {
        return a.execution_order < b.execution_order;
      });
}

void GPUModifierPipeline::allocate_buffers(
    Mesh *mesh_owner, Object *deformed_eval, int vertex_count)
{
  /* Use stable key attached to the original mesh (mesh_owner) */
  const std::string key_buffer_a = "gpu_pipeline_buffer_a";

  /* Try to get existing buffer from mesh GPU cache */
  input_pipeline_buffer_ = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_buffer_a);

  /* Allocate if not present */
  const size_t buffer_size = size_t(vertex_count) * sizeof(float) * 4;

  if (!input_pipeline_buffer_) {
    input_pipeline_buffer_ = bke::BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, deformed_eval, key_buffer_a, buffer_size);

    /* Initialize with REST positions
     * This ensures the first modifier in the
     * pipeline (e.g., SimpleDeform without ShapeKeys)
     * reads valid input data instead of
     * garbage. */
    if (input_pipeline_buffer_) {
      blender::Span<blender::float3> rest_positions = mesh_owner->vert_positions();
      std::vector<float> rest_data(vertex_count * 4);

      for (int v = 0; v < vertex_count; v++) {
        rest_data[v * 4 + 0] = rest_positions[v].x;
        rest_data[v * 4 + 1] = rest_positions[v].y;
        rest_data[v * 4 + 2] = rest_positions[v].z;
        rest_data[v * 4 + 3] = 1.0f; /* Homogeneous coordinate */
      }

      GPU_storagebuf_update(input_pipeline_buffer_, rest_data.data());
    }
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
          hash = BLI_hash_int_2d(hash,
                                 ArmatureSkinningManager::compute_armature_hash(mesh_orig_, amd));
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
          hash = BLI_hash_int_2d(hash,
                                 LatticeSkinningManager::compute_lattice_hash(mesh_orig_, lmd));
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
      case ModifierGPUStageType::SIMPLEDEFORM: {
        /* Simple Deform: Delegate to SimpleDeformManager for complete hash
         * (detects mode, axis, origin, vertex groups) */
        if (mesh_orig_) {
          SimpleDeformModifierData *smd = static_cast<SimpleDeformModifierData *>(
              stage.modifier_data);
          hash = BLI_hash_int_2d(hash,
                                 SimpleDeformManager::compute_simpledeform_hash(mesh_orig_, smd));
        }
        else {
          /* Defensive fallback */
          BLI_assert_unreachable();
          ModifierData *md = static_cast<ModifierData *>(stage.modifier_data);
          hash = BLI_hash_int_2d(hash, uint32_t(md->persistent_uid));
        }
        break;
      }
      case ModifierGPUStageType::HOOK: {
        /* Hook: Delegate to HookManager for complete hash */
        if (mesh_orig_) {
          HookModifierData *hmd = static_cast<HookModifierData *>(stage.modifier_data);
          hash = BLI_hash_int_2d(hash, HookManager::compute_hook_hash(mesh_orig_, hmd));
        }
        else {
          BLI_assert_unreachable();
          ModifierData *md = static_cast<ModifierData *>(stage.modifier_data);
          hash = BLI_hash_int_2d(hash, uint32_t(md->persistent_uid));
        }
        break;
      }
      case ModifierGPUStageType::DISPLACE: {
        /* Hook: Delegate to HookManager for complete hash */
        if (mesh_orig_) {
          DisplaceModifierData *dmd = static_cast<DisplaceModifierData *>(stage.modifier_data);
          hash = BLI_hash_int_2d(hash, DisplaceManager::compute_displace_hash(mesh_orig_, dmd));
        }
        else {
          BLI_assert_unreachable();
          ModifierData *md = static_cast<ModifierData *>(stage.modifier_data);
          hash = BLI_hash_int_2d(hash, uint32_t(md->persistent_uid));
        }
        break;
      }
      case ModifierGPUStageType::WAVE: {
        /* Hook: Delegate to HookManager for complete hash */
        if (mesh_orig_) {
          WaveModifierData *wmd = static_cast<WaveModifierData *>(stage.modifier_data);
          hash = BLI_hash_int_2d(hash, WaveManager::compute_wave_hash(mesh_orig_, wmd));
        }
        else {
          BLI_assert_unreachable();
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
    case ModifierGPUStageType::SIMPLEDEFORM:
      SimpleDeformManager::instance().invalidate_all(mesh_owner);
      break;
    case ModifierGPUStageType::HOOK:
      HookManager::instance().invalidate_all(mesh_owner);
      break;
    case ModifierGPUStageType::DISPLACE:
      DisplaceManager::instance().invalidate_all(mesh_owner);
      break;
    case ModifierGPUStageType::WAVE:
      WaveManager::instance().invalidate_all(mesh_owner);
      break;
    default:
      break;
  }
  /* Invalidation will free input_pipeline_buffer_ via bke::BKE_mesh_gpu_internal_resources_free_for_mesh,
   * Reset it to nullptr so it will be recreated on next frame */
  input_pipeline_buffer_ = nullptr;
}

gpu::StorageBuf *GPUModifierPipeline::execute(
    Mesh *mesh, Object *ob, MeshBatchCache *cache)
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
  allocate_buffers(mesh_owner, ob, vertex_count);

  /* Check if pipeline structure changed (order, add/remove, enable/disable) */
  const uint32_t new_hash = compute_fast_hash();
  if (new_hash != pipeline_hash_) {
    pipeline_hash_ = new_hash;

    /* Pipeline changed -> Invalidate ALL stages (shaders + SSBOs) */
    for (const ModifierGPUStage &stage : stages_) {
      invalidate_stage(stage.type, mesh_owner);
    }
  }

  /* Chain stages: output of stage N becomes input of stage N+1 */
  gpu::StorageBuf *current_buffer = input_pipeline_buffer_;

  for (int stage_idx : stages_.index_range()) {
    const ModifierGPUStage &stage = stages_[stage_idx];

    /* Dispatch stage: manager reads from current_buffer and returns its output buffer.
     * Pass pipeline_hash_ to allow manager to detect changes without recomputing hash. */
    gpu::StorageBuf *result = stage.dispatch_fn(
        mesh, ob, stage.modifier_data, current_buffer, pipeline_hash_);

    if (!result) {
      /* Stage failed, abort pipeline */
      return nullptr;
    }

    /* Use the result as input for the next stage */
    current_buffer = result;
  }
  return current_buffer;
}

void GPUModifierPipeline::clear_stages()
{
  /* Clear only the stages list, preserve pipeline_hash_ for change detection */
  stages_.clear();
  /* Don't touch input_pipeline_buffer_, pipeline_hash_ */
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
                                                 uint32_t pipeline_hash)
{
  /* ShapeKeys are always first, so they don't need input buffer.
   * They compute: output = rest
   * + sum(delta_k * weight_k) */

  Mesh *mesh_eval = id_cast<Mesh *>(ob_eval->data);
  MeshBatchCache *cache = static_cast<MeshBatchCache *>(mesh_eval->runtime->batch_cache);
  if (!cache) {
    return nullptr;
  }

  /* Call existing ShapeKey manager, passing the pipeline hash */
  ShapeKeySkinningManager &sk_mgr = ShapeKeySkinningManager::instance();
  sk_mgr.ensure_static_resources(mesh_orig, pipeline_hash);

  return sk_mgr.dispatch_shapekeys(cache, ob_eval);
}

static gpu::StorageBuf *dispatch_armature_stage(Mesh *mesh_orig,
                                                Object *ob_eval,
                                                void *modifier_data,
                                                gpu::StorageBuf *input,
                                                uint32_t pipeline_hash)
{
  ArmatureModifierData *amd = static_cast<ArmatureModifierData *>(modifier_data);
  if (!amd || !amd->object) {
    return nullptr;
  }

  Mesh *mesh_eval = id_cast<Mesh *>(ob_eval->data);
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
                                               uint32_t pipeline_hash)
{
  LatticeModifierData *lmd = static_cast<LatticeModifierData *>(modifier_data);
  if (!lmd || !lmd->object) {
    return nullptr;
  }

  Mesh *mesh_eval = id_cast<Mesh *>(ob_eval->data);
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

static gpu::StorageBuf *dispatch_simpledeform_stage(Mesh *mesh_orig,
                                                    Object *ob_eval,
                                                    void *modifier_data,
                                                    gpu::StorageBuf *input,
                                                    uint32_t pipeline_hash)
{
  SimpleDeformModifierData *smd = static_cast<SimpleDeformModifierData *>(modifier_data);
  if (!smd) {
    return nullptr;
  }

  Mesh *mesh_eval = id_cast<Mesh *>(ob_eval->data);
  MeshBatchCache *cache = static_cast<MeshBatchCache *>(mesh_eval->runtime->batch_cache);
  if (!cache) {
    return nullptr;
  }

  SimpleDeformManager &sd_mgr = SimpleDeformManager::instance();

  /* Pass smd (original) for settings extraction */
  sd_mgr.ensure_static_resources(smd, ob_eval, mesh_orig, pipeline_hash);

  return sd_mgr.dispatch_deform(smd, DRW_context_get()->depsgraph, ob_eval, cache, input);
}

static gpu::StorageBuf *dispatch_hook_stage(Mesh *mesh_orig,
                                            Object *ob_eval,
                                            void *modifier_data,
                                            gpu::StorageBuf *input,
                                            uint32_t pipeline_hash)
{
  HookModifierData *hmd = static_cast<HookModifierData *>(modifier_data);
  if (!hmd || !hmd->object) {
    return nullptr;
  }

  Mesh *mesh_eval = id_cast<Mesh *>(ob_eval->data);
  MeshBatchCache *cache = static_cast<MeshBatchCache *>(mesh_eval->runtime->batch_cache);
  if (!cache) {
    return nullptr;
  }

  HookManager &hook_mgr = HookManager::instance();

  /* IMPORTANT: hmd comes from ORIGINAL object, so hmd->object is ORIGINAL hook target */
  Object *orig_hook = hmd->object;
  Object *eval_hook = static_cast<Object *>(
      DEG_get_evaluated(DRW_context_get()->depsgraph, orig_hook));

  /* Pass hmd (original) for settings extraction */
  hook_mgr.ensure_static_resources(hmd, orig_hook, ob_eval, mesh_orig, pipeline_hash);

  return hook_mgr.dispatch_deform(
      hmd, DRW_context_get()->depsgraph, eval_hook, ob_eval, cache, input);
}

static gpu::StorageBuf *dispatch_displace_stage(Mesh *mesh_orig,
                                                Object *ob_eval,
                                                void *modifier_data,
                                                gpu::StorageBuf *input,
                                                uint32_t pipeline_hash)
{
  DisplaceModifierData *dmd = static_cast<DisplaceModifierData *>(modifier_data);
  if (!dmd) {
    return nullptr;
  }

  Mesh *mesh_eval = id_cast<Mesh *>(ob_eval->data);
  MeshBatchCache *cache = static_cast<MeshBatchCache *>(mesh_eval->runtime->batch_cache);
  if (!cache) {
    return nullptr;
  }

  DisplaceManager &displace_mgr = DisplaceManager::instance();

  displace_mgr.ensure_static_resources(dmd, ob_eval, mesh_orig, pipeline_hash);

  return displace_mgr.dispatch_deform(
      dmd, DRW_context_get()->depsgraph, ob_eval, cache, input);
}


/** \} */

bool build_gpu_modifier_pipeline(
    Object &ob_eval, Mesh &mesh_orig, GPUModifierPipeline &pipeline)
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
   * This ensures modifier data pointers match what bke::BKE_modifiers_is_deformed_by_*
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
      case eModifierType_SimpleDeform: {
        pipeline.add_stage(ModifierGPUStageType::SIMPLEDEFORM,
                           md,
                           execution_order++,
                           dispatch_simpledeform_stage);
        break;
      }
      case eModifierType_Hook: {
        pipeline.add_stage(ModifierGPUStageType::HOOK, md, execution_order++, dispatch_hook_stage);
        break;
      }
      case eModifierType_Displace: {
        pipeline.add_stage(
            ModifierGPUStageType::DISPLACE, md, execution_order++, dispatch_displace_stage);
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

}  // namespace draw
}  // namespace blender
