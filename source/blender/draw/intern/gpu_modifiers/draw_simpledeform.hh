/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup draw
 *
 * Simple Deform GPU compute (Twist, Bend, Taper, Stretch)
 */

#include <memory>

namespace blender {
struct Depsgraph;
struct Mesh;
struct Object;
struct SimpleDeformModifierData;
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
 * Simple Deform GPU Manager (Singleton)
 * Handles GPU compute for Simple Deform modifier (Twist/Bend/Taper/Stretch)
 */
class SimpleDeformManager {
 public:
  static SimpleDeformManager &instance();

  ~SimpleDeformManager();

  /**
   * Compute a hash of the Simple Deform state to detect changes.
   * 
   * @param mesh_orig The original mesh (for vertex count)
   * @param smd The simple deform modifier data
   * @return Hash value, or 0 if inputs are invalid
   */
  static uint32_t compute_simpledeform_hash(const Mesh *mesh_orig,
                                            const SimpleDeformModifierData *smd);

  /**
   * Prepare CPU-side static resources (vertex group weights).
   * Can be called from extraction phase (non-GL thread).
   * 
   * @param smd The specific SimpleDeformModifierData
   * @param deform_ob The object being deformed
   * @param orig_mesh The original mesh data
   * @param pipeline_hash Hash for change detection
   */
  void ensure_static_resources(const SimpleDeformModifierData *smd,
                               Object *deform_ob,
                               Mesh *orig_mesh,
                               uint32_t pipeline_hash);

  /**
   * Execute simple deform compute shader. Must be called from GL context.
   * Returns SSBO containing deformed positions.
   * 
   * @param smd The specific SimpleDeformModifierData
   * @param depsgraph Dependency graph
   * @param deformed_eval Evaluated object being deformed
   * @param cache Mesh batch cache
   * @param ssbo_in Input positions SSBO (from previous stage)
   * @return Output SSBO with deformed positions, or nullptr on failure
   */
  gpu::StorageBuf *dispatch_deform(const SimpleDeformModifierData *smd,
                                   Depsgraph *depsgraph,
                                   Object *deformed_eval,
                                   MeshBatchCache *cache,
                                   gpu::StorageBuf *ssbo_in);

  /**
   * Free CPU-side static data for a specific mesh.
   */
  void free_resources_for_mesh(Mesh *mesh);

  /**
   * Invalidate GPU resources (shaders + SSBOs) for a specific mesh.
   * Called when mesh topology/modifier settings change.
   */
  void invalidate_all(Mesh *mesh);

  /**
   * Free all CPU-side static data.
   */
  void free_all();

 private:
  SimpleDeformManager();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace draw
}  // namespace blender
