/* SPDX-FileCopyrightText:2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <memory>

namespace blender {
struct Depsgraph;
struct Mesh;
struct Object;
struct ArmatureModifierData;
}  // namespace blender

namespace blender {
namespace gpu {
class StorageBuf;
}  // namespace gpu
}  // namespace blender

namespace blender {
namespace draw {

struct MeshBatchCache;

class ArmatureSkinningManager {
 public:
  static ArmatureSkinningManager &instance();

  ArmatureSkinningManager();
  ~ArmatureSkinningManager();

  /**
   * Compute a hash of the Armature deformation state to detect changes.
   * Includes: vertex count, armature pointer, vertex group samples.
   * 
   * @param mesh_orig The original mesh (for vertex count)
   * @param amd The armature modifier data (contains armature pointer, DQS flag, etc.)
   * @return Hash value, or 0 if inputs are invalid
   */
  static uint32_t compute_armature_hash(const Mesh *mesh_orig,
                                        const ArmatureModifierData *amd);

  /**
   * Prepare CPU-only static resources (indices/weights/rest positions).
   * Can be called from extraction phase (non-GL thread).
   *
   * @param amd The specific ArmatureModifierData to extract settings from
   * @param arm_ob The armature object (original)
   * @param deformed_ob The object being deformed
   * @param orig_mesh The original mesh data
   * @param pipeline_hash Hash for change detection (computed by GPUModifierPipeline)
   */
  void ensure_static_resources(const ArmatureModifierData *amd,
                               Object *arm_ob,
                               Object *deformed_ob,
                               Mesh *orig_mesh,
                               uint32_t pipeline_hash);

  /**
   * Execute the skinning compute. Must be called from GL context.
   * Returns an SSBO containing the skinned positions on success.
   * 
   * @param amd The specific ArmatureModifierData to extract settings from
   */
  gpu::StorageBuf *dispatch_skinning(const ArmatureModifierData *amd,
                                              Depsgraph *depsgraph,
                                              Object *eval_armature,
                                              Object *deformed_eval,
                                              MeshBatchCache *cache,
                                              gpu::StorageBuf *ssbo_in = nullptr);

  /* Free resources associated to a specific mesh. */
  void free_resources_for_mesh(Mesh *mesh);

  /* Invalidate all GPU resources (shaders + SSBOs) for a specific mesh.
   * This marks the mesh for full GPU resource recreation on next dispatch. */
  void invalidate_all(Mesh *mesh);

  /* Free all resources. */
  void free_all();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace draw
}  // namespace blender
