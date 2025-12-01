/* SPDX-FileCopyrightText:2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <memory>

/* Forward declarations to keep header lightweight. */
struct Object;
struct Mesh;
struct Depsgraph;

namespace blender {
namespace gpu {
class VertBuf;
class StorageBuf;
}  // namespace gpu
}  // namespace blender

namespace blender::draw {
struct MeshBatchCache;

class ArmatureSkinningManager {
 public:
  static ArmatureSkinningManager &instance();

  ArmatureSkinningManager();
  ~ArmatureSkinningManager();

  /* Prepare CPU-only static resources (indices/weights/rest positions).
   * Can be called from extraction phase (non-GL thread). */
  void ensure_static_resources(Object *arm_ob, Object *deformed_ob, Mesh *orig_mesh);

  /* Execute the skinning compute. Must be called from GL context.
   * Returns an SSBO containing the skinned positions on success (either the provided `ssbo_in`
   * or an internal SSBO). Returns nullptr on failure. The caller should perform the final
   * scatter-to-corners when chaining multiple deformers. */
  blender::gpu::StorageBuf *dispatch_skinning(Depsgraph *depsgraph,
                                              Object *eval_armature,
                                              Object *deformed_eval,
                                              MeshBatchCache *cache,
                                              blender::gpu::StorageBuf *ssbo_in = nullptr);

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

}  // namespace blender::draw
