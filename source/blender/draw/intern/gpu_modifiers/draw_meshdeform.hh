/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <memory>

namespace blender {
struct Depsgraph;
struct Mesh;
struct Object;
struct MeshDeformModifierData;
}  // namespace blender

namespace blender {
namespace gpu {
class StorageBuf;
}  // namespace gpu
}  // namespace blender

namespace blender {
namespace draw {

/* Forward declaration for MeshBatchCache */
struct MeshBatchCache;
/**
 * Manager for GPU-accelerated Mesh Deform modifier.
 *
 * Handles:
 * - CPU caching of static bind data (offsets, influences, bind cage coords)
 * - Per-frame upload of cage vertex deltas
 * - Compute shader dispatch for mesh deformation
 */
class MeshDeformSkinningManager {
 public:
  struct Impl;

  static MeshDeformSkinningManager &instance();

  ~MeshDeformSkinningManager();

  /**
   * Compute a hash of the Mesh Deform state to detect changes.
   * Includes: vertex count, cage vertex count, bind data, vertex group.
   *
   * @param mesh_orig The original mesh (for vertex count)
   * @param mmd The mesh deform modifier data
   * @return Hash value, or 0 if inputs are invalid
   */
  static uint32_t compute_meshdeform_hash(const Mesh *mesh_orig,
                                          const MeshDeformModifierData *mmd);

  /**
   * Prepare CPU-side static resources (bind offsets, influences, bind cage coords).
   * Can be called from extraction phase (non-GL thread).
   *
   * @param mmd The specific MeshDeformModifierData to extract settings from
   * @param cage_ob The cage mesh object
   * @param deformed_ob The object being deformed
   * @param orig_mesh The original mesh data
   * @param pipeline_hash Hash for change detection
   */
  void ensure_static_resources(const MeshDeformModifierData *mmd,
                               Object *cage_ob,
                               Object *deformed_ob,
                               Mesh *orig_mesh,
                               uint32_t pipeline_hash);

  /**
   * Execute mesh deform compute shader.
   * Reads from ssbo_in (previous stage output), writes to internal SSBO.
   * Returns SSBO containing deformed positions.
   *
   * @param mmd The specific MeshDeformModifierData to extract settings from
   */
  gpu::StorageBuf *dispatch_deform(const MeshDeformModifierData *mmd,
                                   Depsgraph *depsgraph,
                                   Object *eval_cage,
                                   Object *deformed_eval,
                                   MeshBatchCache *cache,
                                   gpu::StorageBuf *ssbo_in);

  /**
   * Set debug logging enable flag. Used for runtime diagnostics.
   */
  void set_debug_enabled(bool enabled);

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
  MeshDeformSkinningManager();
  std::unique_ptr<Impl> impl_;
};

}  // namespace draw
}  // namespace blender
