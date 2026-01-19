/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <memory>

namespace blender {
struct Depsgraph;
struct Mesh;
struct Object;
struct CastModifierData;
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
 * Minimal CastManager following HookManager pattern.
 * Provides plumbing for future GPU implementation. The dispatch is a
 * no-op (returns input SSBO) so callers can integrate incrementally.
 */
class CastManager {
 public:
  struct Impl;

  static CastManager &instance();

  ~CastManager();

  static uint32_t compute_cast_hash(const Mesh *mesh_orig, const CastModifierData *cmd);

  void ensure_static_resources(const CastModifierData *cmd,
                               Object *ctrl_ob,
                               Object *deformed_ob,
                               Mesh *orig_mesh,
                               uint32_t pipeline_hash);

  gpu::StorageBuf *dispatch_deform(const CastModifierData *cmd,
                                   Depsgraph *depsgraph,
                                   Object *deformed_eval,
                                   MeshBatchCache *cache,
                                   gpu::StorageBuf *ssbo_in);

  void free_resources_for_mesh(Mesh *mesh);
  void invalidate_all(Mesh *mesh);
  void free_all();

 private:
  CastManager();
  std::unique_ptr<Impl> impl_;
};

}  // namespace draw
}  // namespace blender
