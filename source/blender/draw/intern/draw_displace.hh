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

#include "BLI_map.hh"
#include "BLI_vector.hh"

#include "DNA_modifier_types.h"

#include "draw_cache_extract.hh"

#include <memory>

struct Depsgraph;
struct DisplaceModifierData;
struct Mesh;
struct MeshBatchCache;
struct Object;

namespace blender {
namespace gpu {
class Shader;
class StorageBuf;
}  // namespace gpu
}  // namespace blender

namespace blender::draw {

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
  uint32_t compute_displace_hash(const Mesh *mesh_orig, const DisplaceModifierData *dmd);

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
  blender::gpu::StorageBuf *dispatch_deform(const DisplaceModifierData *dmd,
                                            Depsgraph *depsgraph,
                                            Object *deformed_eval,
                                            MeshBatchCache *cache,
                                            blender::gpu::StorageBuf *ssbo_in);

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
