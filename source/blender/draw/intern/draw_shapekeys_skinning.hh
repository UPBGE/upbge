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
  void ensure_static_resources(Object *ob_eval, Mesh *orig_mesh);

  /* Execute shape-key blending compute + scatter. Must be called from GL context (draw pass).
   * If `ssbo_out` is provided the result will be written into that buffer instead of an
   * internally managed buffer. If `do_scatter` is false, no scatter-to-corners will be
   * performed (useful to chain multiple deformers and scatter once at the end). */
  bool dispatch_shapekeys(Depsgraph *depsgraph,
                          Object *ob_eval,
                          struct MeshBatchCache *cache,
                          blender::gpu::VertBuf *vbo_pos,
                          blender::gpu::VertBuf *vbo_nor,
                          blender::gpu::StorageBuf *ssbo_out = nullptr,
                          bool do_scatter = true);

  /* Free resources associated to a specific mesh (CPU-side). GPU resources freed by BKE mesh GPU
   * cache. */
  void free_resources_for_mesh(Mesh *mesh);

  /* Free all CPU-side resources. */
  void free_all();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} /* namespace draw */
} /* namespace blender */
