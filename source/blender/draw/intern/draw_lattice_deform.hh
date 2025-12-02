/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "GPU_storage_buffer.hh"

#include "draw_cache_extract.hh"

#include "BKE_modifier.hh"

struct Depsgraph;
struct Mesh;
struct Object;

namespace blender::draw {

/**
 * Manager for GPU-accelerated Lattice deformation.
 *
 * Handles:
 * - CPU calculation of lattice control point influences
 * - GPU upload of control points and deformation parameters
 * - Compute shader dispatch for trilinear interpolation
 */
class LatticeSkinningManager {
 public:
  struct Impl;

  static LatticeSkinningManager &instance();

  ~LatticeSkinningManager();

  /**
   * Compute a hash of the Lattice deformation state to detect changes.
   * Includes: vertex count, lattice dimensions, control point count.
   */
  static uint32_t compute_lattice_hash(const Mesh *mesh, const Object *ob);

  /**
   * Prepare CPU-side static resources (lattice control points, grid dimensions).
   * Can be called from extraction phase (non-GL thread).
   * 
   * @param lmd The specific LatticeModifierData to extract settings from
   * @param lattice_ob The lattice object
   * @param deformed_ob The object being deformed
   * @param orig_mesh The original mesh data
   * @param pipeline_hash Hash for change detection
   */
  void ensure_static_resources(const LatticeModifierData *lmd,
                               Object *lattice_ob,
                               Object *deformed_ob,
                               Mesh *orig_mesh,
                               uint32_t pipeline_hash);

  /**
   * Execute lattice deformation compute shader.
   * Reads from ssbo_in (previous stage output), writes to internal SSBO.
   * Returns SSBO containing deformed positions.
   * 
   * @param lmd The specific LatticeModifierData to extract settings from
   */
  blender::gpu::StorageBuf *dispatch_deform(const LatticeModifierData *lmd,
                                            Depsgraph *depsgraph,
                                            Object *eval_lattice,
                                            Object *deformed_eval,
                                            MeshBatchCache *cache,
                                            blender::gpu::StorageBuf *ssbo_in);

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
  LatticeSkinningManager();
  std::unique_ptr<Impl> impl_;
};

}  // namespace blender::draw
