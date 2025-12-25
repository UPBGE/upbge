/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "BKE_mesh.hh"
#include "BKE_mesh_gpu.hh"
#include "BKE_ocean.h"

#include "DNA_object_types.h"

namespace blender {
namespace bke {

struct MeshGpuData {
  blender::bke::MeshGPUTopology topology;
  /* Optional internal resources container (owned here). */
  blender::bke::MeshGpuInternalResources *internal_resources = nullptr;

  /* Debug: session UUID of the owning Mesh for validation (0 = uninitialized) */
  uint32_t session_uid = 0;
};

/* Manager owning mesh/armature GPU caches. */
class MeshGPUCacheManager {
 public:
  static MeshGPUCacheManager &get();

  /* Global frees. */
  void free_all();

  /* Per-mesh operations. */
  void free_for_mesh(struct Mesh *mesh);
  MeshGpuInternalResources *mesh_internal_resources_ensure(struct Mesh *mesh);

  /* Ocean helpers (INTERNAL SSBOs ONLY, not exposed to Python wrappers). */
  blender::gpu::StorageBuf *ocean_internal_ssbo_ensure(struct Ocean *ocean,
                                                       const std::string &key,
                                                       size_t size);
  blender::gpu::StorageBuf *ocean_internal_ssbo_get(struct Ocean *ocean, const std::string &key);
  void ocean_internal_ssbo_release(struct Ocean *ocean, const std::string &key);
  void ocean_internal_ssbo_detach(struct Ocean *ocean, const std::string &key);
  /* Free all SSBOs cached for a single Ocean owner. */
  void free_ocean_cache(struct Ocean *ocean);
  void free_all_ocean_caches();

  /* Accessors for existing code to migrate gradually. */
  std::unordered_map<const Mesh *, MeshGpuData> &mesh_cache();
  std::vector<MeshGpuData> &orphans();
  std::mutex &mutex();

  /* Flush orphans while GL context is active. */
  void flush_orphans();

  /* Release CPU-side memory held by the containers (call after GPU frees). */
  void release_cpu_memory();

 private:
  MeshGPUCacheManager() = default;
  ~MeshGPUCacheManager() = default;
  MeshGPUCacheManager(const MeshGPUCacheManager &) = delete;
  MeshGPUCacheManager &operator=(const MeshGPUCacheManager &) = delete;

  /* Owned containers. */
  std::unordered_map<const Mesh *, MeshGpuData> g_mesh_data_cache_;
  std::vector<MeshGpuData> g_mesh_data_orphans_;
  std::mutex g_mesh_cache_mutex_;

  /* Ocean: map owner -> { key -> (ssbo, capacity_bytes) } */
  struct InternalSSBOEntry {
    blender::gpu::StorageBuf *ssbo = nullptr;
    size_t capacity = 0;
  };
  std::unordered_map<const Ocean *, std::unordered_map<std::string, InternalSSBOEntry>>
      g_ocean_gpu_ssbos_;
};

}  // namespace bke
}  // namespace blender
