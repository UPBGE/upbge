/* SPDX-FileCopyrightText:2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_map.hh"
#include "BLI_vector.hh"

#include "BKE_mesh.hh"

#include "GPU_vertex_buffer.hh"

namespace blender {
namespace gpu {
class StorageBuf;
} // namespace gpu
namespace draw {

class ShapeKeySkinningManager {
 public:
  static ShapeKeySkinningManager &instance();

  ShapeKeySkinningManager();
  ~ShapeKeySkinningManager();

  /* Prepare CPU-only static resources (deltas, rest positions). Safe to call from extraction
   * thread. */
  void ensure_static_resources(Mesh *orig_mesh);

  /* Execute shape-key blending compute. Must be called from GL context (draw pass).
   * Returns an SSBO containing the skinned positions on success (either the provided `ssbo_out`
   * or an internal SSBO). Returns nullptr on failure. The caller should perform final
   * scatter-to-corners when chaining deformers. */
  blender::gpu::StorageBuf *dispatch_shapekeys(struct MeshBatchCache *cache,
                                               struct Object *ob_eval,
                                               blender::gpu::StorageBuf *ssbo_out = nullptr);

  /* Free resources associated to a specific mesh (CPU-side). GPU resources freed by BKE mesh GPU
   * cache. */
  void free_resources_for_mesh(Mesh *mesh);

  /* Invalidate all GPU resources (shaders + SSBOs) for a specific mesh.
   * This marks the mesh for full GPU resource recreation on next dispatch. */
  void invalidate_all(Mesh *mesh);

  /* Free all CPU-side resources. */
  void free_all();

  /**
   * Compute a hash of the ShapeKey state to detect changes.
   * Includes: vertex count, keyblock count, Basis, Relative To, Edit Mode changes.
   * Returns 0 if mesh has no ShapeKeys.
   */
  static uint32_t compute_shapekey_hash(const Mesh *mesh);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} /* namespace draw */
} /* namespace blender */
