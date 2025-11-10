/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "BKE_mesh_gpu.hh"

#include "BKE_mesh.hh"
#include "DNA_object_types.h"

namespace blender {
namespace bke {

struct MeshGpuData {
  blender::bke::MeshGPUTopology topology;
  /* Support multiple compute shaders per mesh keyed by hash of generated source. */
  std::unordered_map<size_t, blender::gpu::Shader *> compute_shaders;
  /* Optional internal resources container (owned here). */
  blender::bke::MeshGpuInternalResources *internal_resources = nullptr;
};

/* Manager owning mesh/armature GPU caches. */
class MeshGPUCacheManager {
 public:
  static MeshGPUCacheManager &get();

  /* Global frees. */
  void free_all();
  void free_all_armature_caches();

  /* Per-mesh operations. */
  void free_for_mesh(struct Mesh *mesh);
  MeshGpuInternalResources *mesh_internal_resources_ensure(struct Mesh *mesh);

  /* Armature helpers. */
  blender::gpu::StorageBuf *armature_internal_ssbo_ensure(struct Object *arm,
                                                          const std::string &key,
                                                          size_t size);
  blender::gpu::StorageBuf *armature_internal_ssbo_get(struct Object *arm, const std::string &key);
  void armature_internal_ssbo_release(struct Object *arm, const std::string &key);

  /* Accessors for existing code to migrate gradually. */
  std::unordered_map<const Mesh *, MeshGpuData> &mesh_cache();
  std::vector<MeshGpuData> &orphans();
  std::mutex &mutex();
  std::unordered_map<const Object *, blender::bke::MeshGpuInternalResources> &armature_resources();

  /* Flush orphans while GL context is active. */
  void flush_orphans();

 private:
  MeshGPUCacheManager() = default;
  ~MeshGPUCacheManager() = default;
  MeshGPUCacheManager(const MeshGPUCacheManager &) = delete;
  MeshGPUCacheManager &operator=(const MeshGPUCacheManager &) = delete;

  /* Owned containers. */
  std::unordered_map<const Mesh *, MeshGpuData> g_mesh_data_cache_;
  std::vector<MeshGpuData> g_mesh_data_orphans_;
  std::mutex g_mesh_cache_mutex_;
  std::unordered_map<const Object *, blender::bke::MeshGpuInternalResources>
      g_armature_gpu_resources_;
};

}  // namespace bke
}  // namespace blender
