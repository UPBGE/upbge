/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <memory>

namespace blender {
namespace gpu {
class StorageBuf;
}  // namespace gpu
}  // namespace blender

namespace blender {
struct Depsgraph;
struct Mesh;
struct Object;
struct WaveModifierData;
}  // namespace blender

namespace blender::draw {

struct MeshBatchCache;

class WaveManager {
 public:
  static WaveManager &instance();

  WaveManager();
  ~WaveManager();

    /**
   * Compute hash for Wave modifier pipeline.
   * Only hashes static topology/config, NOT runtime uniforms.
   */
  static uint32_t compute_wave_hash(const Mesh *mesh_orig, const WaveModifierData *dmd);

  /* Ensure any cached/static resources required by a Wave modifier instance. */
  void ensure_static_resources(const WaveModifierData *wmd, Object *deform_ob, Mesh *orig_mesh, uint32_t pipeline_hash);

  /* Dispatch GPU compute for the Wave modifier; returns an SSBO with deformed positions. */
  gpu::StorageBuf *dispatch_deform(const WaveModifierData *wmd,
                                   Depsgraph *depsgraph,
                                   Object *deformed_eval,
                                   MeshBatchCache *cache,
                                   gpu::StorageBuf *ssbo_in);

  /* Free per-mesh cached GPU resources. */
  void free_resources_for_mesh(Mesh *mesh);
  void invalidate_all(Mesh *mesh);
  void free_all();

 private:
  struct Impl;
  Impl *impl_;
};

} // namespace blender::draw
