/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "GPU_storage_buffer.hh"

#include "draw_cache_extract.hh"

#include "BKE_modifier.hh"

struct Depsgraph;
struct Mesh;
struct Object;

namespace blender::draw {

/**
 * Manager for GPU-accelerated Hook deformation.
 *
 * Handles:
 * - CPU calculation of transformation matrices (hook object → deformed object space)
 * - GPU upload of hook parameters (center, falloff, force, curve falloff LUT)
 * - Compute shader dispatch for distance-based vertex transformation with falloff
 */
class HookManager {
 public:
  struct Impl;

  static HookManager &instance();

  ~HookManager();

  /**
   * Compute a hash of the Hook deformation state to detect changes.
   * Includes: vertex count, hook object pointer, bone name, falloff type, vertex groups.
   * 
   * @param mesh_orig The original mesh (for vertex count)
   * @param hmd The hook modifier data (contains object, bone, falloff, etc.)
   * @return Hash value, or 0 if inputs are invalid
   */
  static uint32_t compute_hook_hash(const Mesh *mesh_orig, const HookModifierData *hmd);

  /**
   * Prepare CPU-side static resources (vertex group weights, falloff curve LUT).
   * Can be called from extraction phase (non-GL thread).
   * 
   * Uses simple property comparison to detect changes instead of complex hashing.
   * Compares: curfalloff pointer, force, falloff, falloff_type, and flags.
   * This automatically detects curve edits (pointer changes when curve is modified).
   * 
   * @param hmd The specific HookModifierData to extract settings from
   * @param hook_ob The hook object (or armature if using bone)
   * @param deformed_ob The object being deformed
   * @param orig_mesh The original mesh data
   * @param pipeline_hash Unused (kept for API compatibility - now uses direct property comparison)
   */
  void ensure_static_resources(const HookModifierData *hmd,
                               Object *hook_ob,
                               Object *deformed_ob,
                               Mesh *orig_mesh,
                               uint32_t pipeline_hash);

  /**
   * Execute hook deformation compute shader.
   * Reads from ssbo_in (previous stage output), writes to internal SSBO.
   * Returns SSBO containing deformed positions.
   * 
   * @param hmd The specific HookModifierData to extract settings from
   * @param depsgraph The dependency graph
   * @param eval_hook The evaluated hook object
   * @param deformed_eval The evaluated deformed object
   * @param cache Mesh batch cache
   * @param ssbo_in Input position buffer from previous stage
   */
  blender::gpu::StorageBuf *dispatch_deform(const HookModifierData *hmd,
                                            Depsgraph *depsgraph,
                                            Object *eval_hook,
                                            Object *deformed_eval,
                                            MeshBatchCache *cache,
                                            blender::gpu::StorageBuf *ssbo_in);

  /**
   * Free all GPU resources associated with a mesh.
   */
  void free_resources_for_mesh(Mesh *mesh);

  /**
   * Invalidate all GPU resources for a mesh (triggers recreation).
   */
  void invalidate_all(Mesh *mesh);

  /**
   * Free all cached resources (called on module exit).
   */
  void free_all();

 private:
  HookManager();
  std::unique_ptr<Impl> impl_;
};

}  // namespace blender::draw
