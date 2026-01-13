/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
* \ingroup draw
*
* GPU-accelerated Displace modifier implementation.
* 
* Supported features:
* - Direction: X, Y, Z
* - Space: Local, Global
* - Vertex group masking
* - Strength and midlevel parameters
* 
* Partial support (limitations):
* - Direction: Normal (uses ORIGINAL normals, not deformed)
*   → Will not follow deformations from previous modifiers
*   → Use CPU fallback for accurate normal-based displacement
* 
* NOT supported (for now):
* - Procedural textures (only image textures supported)
* - Custom normals (requires corner normals)
*/

#pragma once

#include <memory>

namespace blender {
struct Depsgraph;
struct Mesh;
struct Object;
struct DisplaceModifierData;
}  // namespace blender

namespace blender {
namespace gpu {
class StorageBuf;
}  // namespace gpu
}  // namespace blender

namespace blender::draw {

/* Forward declaration for MeshBatchCache */
struct MeshBatchCache;

/**
 * Singleton manager for GPU-based Displace modifier.
 * Handles resource management and compute shader dispatch.
 */
class DisplaceManager {
 public:
  static DisplaceManager &instance();

  /**
   * Compute hash for Displace modifier pipeline.
   * Only hashes static topology/config, NOT runtime uniforms (strength, midlevel).
   */
  static uint32_t compute_displace_hash(const Mesh *mesh_orig, const DisplaceModifierData *dmd);

  /**
   * Ensure static resources (vertex group weights) are up-to-date.
   * Called once per frame before dispatch_deform().
   */
  void ensure_static_resources(const DisplaceModifierData *dmd,
                               Object *deform_ob,
                               Mesh *orig_mesh,
                               uint32_t pipeline_hash);

  /**
   * Dispatch GPU compute shader to deform mesh vertices.
   * Returns SSBO containing deformed positions (vec4 per vertex).
   */
  gpu::StorageBuf *dispatch_deform(const DisplaceModifierData *dmd,
                                   Depsgraph *depsgraph,
                                   Object *deformed_eval,
                                   MeshBatchCache *cache,
                                   gpu::StorageBuf *ssbo_in);

  /**
   * Free all cached resources associated with a specific mesh.
   * Called when mesh is deleted or batch cache invalidated.
   */
  void free_resources_for_mesh(Mesh *mesh);

  /**
   * Invalidate cached resources for a mesh (mark for recomputation).
   * Called when mesh topology changes.
   */
  void invalidate_all(Mesh *mesh);

  /**
   * Free all cached resources (called on exit or context switch).
   */
  void free_all();

 private:
  DisplaceManager();
  ~DisplaceManager();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace blender::draw
