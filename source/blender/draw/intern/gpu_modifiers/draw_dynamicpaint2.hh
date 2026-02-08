/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * GPU-accelerated Dynamic Paint 2 modifier manager.
 *
 * Architecture:
 *   One compute shader is compiled once per modifier instance.
 *   For each brush in the modifier's ListBase, the manager does
 *   one GPU dispatch with different uniforms (origin, target,
 *   direction, radius, intensity, texture binding).
 *   This avoids shader recompilation for N brushes.
 *
 *   Texture support uses gpu_shader_common_texture_lib.cc
 *   (same infrastructure as draw_displace / draw_wave / draw_warp).
 */

#pragma once

#include <memory>

namespace blender {
struct Depsgraph;
struct Mesh;
struct Object;
struct DynamicPaint2GpuModifierData;
}  // namespace blender

namespace blender {
namespace gpu {
class StorageBuf;
}  // namespace gpu
}  // namespace blender

namespace blender::draw {

struct MeshBatchCache;

/**
 * Singleton manager for GPU Dynamic Paint 2 modifier.
 * Handles resource management and compute shader dispatch.
 */
class DynamicPaint2GpuManager {
 public:
  static DynamicPaint2GpuManager &instance();

  /**
   * Compute hash for pipeline change detection.
   * Hashes brush count, brush UIDs, origin/target pointers,
   * direction modes, texture pointers — anything that would
   * require shader or resource reallocation.
   * Does NOT hash runtime uniforms (radius, intensity) that
   * change per-frame without structural impact.
   */
  static uint32_t compute_dp2gpu_hash(const Mesh *mesh_orig,
                                      const DynamicPaint2GpuModifierData *pmd);

  /**
   * Ensure static resources are up-to-date for the current frame.
   * Called once per frame before dispatch_deform().
   */
  void ensure_static_resources(const DynamicPaint2GpuModifierData *pmd,
                               Object *deform_ob,
                               Mesh *orig_mesh,
                               uint32_t pipeline_hash);

  /**
   * Dispatch GPU compute shader(s) to deform mesh vertices.
   * Iterates all brushes in pmd->brushes, doing one dispatch per brush.
   * Returns SSBO containing final deformed positions (vec4 per vertex).
   */
  gpu::StorageBuf *dispatch_deform(const DynamicPaint2GpuModifierData *pmd,
                                   Depsgraph *depsgraph,
                                   Object *deformed_eval,
                                   MeshBatchCache *cache,
                                   gpu::StorageBuf *ssbo_in);

  /**
   * Free all cached resources associated with a specific mesh.
   */
  void free_resources_for_mesh(Mesh *mesh);

  /**
   * Invalidate cached resources for a mesh (mark for recomputation).
   */
  void invalidate_all(Mesh *mesh);

  /**
   * Free all cached resources (called on exit or context switch).
   */
  void free_all();

 private:
  DynamicPaint2GpuManager();
  ~DynamicPaint2GpuManager();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace blender::draw
