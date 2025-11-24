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

  /* Execute the skinning compute + scatter. Must be called from GL context.
   * Returns true on success; false if a fallback (CPU) should be used.
   * If `ssbo_in` is provided, it will be used as the rest positions input (useful when
   * chaining deformers). If `do_scatter` is false, no scatter-to-corners will be performed and
   * the skinned positions will remain in the internal SSBO (useful to chain additional
   * deformers and scatter once at the end). */
  bool dispatch_skinning(Depsgraph *depsgraph,
                         Object *armature,
                         Object *deformed_eval,
                         MeshBatchCache *cache,
                         blender::gpu::VertBuf *vbo_pos,
                         blender::gpu::VertBuf *vbo_nor,
                         blender::gpu::StorageBuf *ssbo_in = nullptr,
                         bool do_scatter = true);

  /* Free resources associated to a specific mesh. */
  void free_resources_for_mesh(Mesh *mesh);

  /* Free all resources. */
  void free_all();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace blender::draw
