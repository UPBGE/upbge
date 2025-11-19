/* SPDX-FileCopyrightText:2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ShapeKey GPU blending manager (squelette)
 */

#pragma once

#include "BLI_map.hh"
#include "BLI_vector.hh"

#include "BKE_mesh.hh"

#include "GPU_vertex_buffer.hh"

namespace blender {
namespace draw {

class ShapeKeySkinningManager {
 public:
  static ShapeKeySkinningManager &instance();

  ShapeKeySkinningManager();
  ~ShapeKeySkinningManager();

  /* Prepare CPU-only static resources (deltas, rest positions). Safe to call from extraction
   * thread. */
  void ensure_static_resources(Object *ob_eval, Mesh *orig_mesh);

  /* Execute shape-key blending compute + scatter. Must be called from GL context (draw pass). */
  bool dispatch_shapekeys(Depsgraph *depsgraph,
                          Object *ob_eval,
                          struct MeshBatchCache *cache,
                          blender::gpu::VertBuf *vbo_pos,
                          blender::gpu::VertBuf *vbo_nor);

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
