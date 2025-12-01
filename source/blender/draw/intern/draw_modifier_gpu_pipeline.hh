/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_modifier.hh"
#include "BLI_vector.hh"
#include "GPU_storage_buffer.hh"

#include "draw_cache_extract.hh"

struct Mesh;
struct Object;
struct MeshBatchCache;

namespace blender::draw {

/**
 * GPU Modifier Pipeline - Chains deform modifiers on GPU
 * 
 * Design goals:
 * - Maintain CPU execution order (shapekeys → armature → lattice → ...)
 * - Ping-pong buffers between stages to avoid redundant copies
 * - Recompile shaders only when modifier stack changes
 * - Support heterogeneous modifier types
 */

enum class ModifierGPUStageType : uint8_t {
  SHAPEKEYS = 0,
  ARMATURE = 1,
  LATTICE = 2,
  CURVE = 3,
  SIMPLE_DEFORM = 4,
  WAVE = 5,
  HOOK = 6,
  /* Add new deform modifiers here */
  CUSTOM = 255
};

typedef struct ModifierGPUStage {
  ModifierGPUStageType type;
  void *modifier_data; /* ModifierData* or Key* */
  int execution_order; /* Lower = earlier execution */
  
  /* Stage-specific dispatch function */
  using DispatchFunc = gpu::StorageBuf *(*)(
      Mesh *mesh,
      Object *ob,
      void *modifier_data,
      gpu::StorageBuf *input_positions,
      gpu::StorageBuf *output_positions);
  
  DispatchFunc dispatch_fn;
} ModifierGPUStage;

class GPUModifierPipeline {
private:
 Vector<ModifierGPUStage> stages_;
  
 /* Working buffer for pipeline (pre-filled with rest positions) */
 gpu::StorageBuf *buffer_a_ = nullptr;
  
 /* Shader cache invalidation - hybrid hash system */
 uint32_t pipeline_hash_ = 0;
 bool needs_recompile_ = false;
  
public:
 GPUModifierPipeline() = default;
 ~GPUModifierPipeline();
  
  /**
   * Add a modifier stage to the pipeline.
   * Stages are automatically sorted by execution_order.
   */
  void add_stage(ModifierGPUStageType type,
                 void *modifier_data,
                 int execution_order,
                 ModifierGPUStage::DispatchFunc dispatch_fn);
  
  /**
   * Execute the full modifier pipeline.
   * Returns the final output buffer (positions).
   */
  gpu::StorageBuf *execute(Mesh *mesh, Object *ob, MeshBatchCache *cache);
  
  /**
   * Clear all stages (called when modifier stack changes).
   */
  void clear();

  /**
   * Clear only the stages list (preserves pipeline_hash_ for change detection).
   * Used by build_gpu_modifier_pipeline to rebuild the stages without losing hash state.
   */
  void clear_stages();

  /**
   * Check if pipeline needs shader recompilation.
   */
  bool needs_shader_recompile() const { return needs_recompile_; }
  
  /**
   * Mark shaders as dirty (e.g., when modifier settings change).
   */
  void invalidate_shaders();
  
  /**
   * Get the number of stages in the pipeline.
   */
  int stage_count() const { return stages_.size(); }
  
  /**
   * Get the current pipeline hash (for debugging).
   */
  uint32_t get_pipeline_hash() const { return pipeline_hash_; }
 private:
  void sort_stages();
  void allocate_buffers(Mesh *mesh_owner, int vertex_count);
  /**
   * Compute fast hash to detect pipeline structure changes.
   * Includes:
   * - ShapeKeys: Key pointer, deform_method, totkey, type, execution_order
   * - Modifiers: persistent_uid, type, mode, execution_order
   */
  uint32_t compute_fast_hash() const;
  /**
   * Invalidate all GPU resources (shaders + SSBOs) for a specific stage.
   * This triggers full recreation on next frame.
   */
  void invalidate_stage(ModifierGPUStageType type, Mesh *mesh_owner);
};

/**
 * Build the GPU modifier pipeline from an Object's modifier stack.
 * Only adds modifiers that:
 * - Are enabled and visible in viewport
 * - Request GPU execution (ARM_DEFORM_METHOD_GPU, KEY_DEFORM_METHOD_GPU, etc.)
 * - Are deform-only (no topology changes)
 * 
 * Returns true if at least one modifier was added.
 */
bool build_gpu_modifier_pipeline(Object &ob_eval, Mesh &mesh_orig, GPUModifierPipeline &pipeline);

}  // namespace blender::draw
