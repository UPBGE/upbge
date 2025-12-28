/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "mesh_gpu_cache.hh"

#include "BKE_mesh.hh"
#include "BKE_mesh_gpu.hh"

#include "GPU_storage_buffer.hh"

#include <mutex>

namespace blender {
namespace bke {

MeshGPUCacheManager &MeshGPUCacheManager::get()
{
  static MeshGPUCacheManager instance;
  return instance;
}

void MeshGPUCacheManager::free_all()
{
  BKE_mesh_gpu_free_all_caches();
  /* Also free Ocean internal buffers to keep global frees coherent. */
  free_all_ocean_caches();
}

void MeshGPUCacheManager::free_for_mesh(struct Mesh *mesh)
{
  BKE_mesh_gpu_free_for_mesh(mesh);
}

/* ---------------- Ocean internal SSBOs (not exposed to Python) ---------------- */

blender::gpu::StorageBuf *MeshGPUCacheManager::ocean_internal_ssbo_ensure(struct Ocean *ocean,
                                                                          const std::string &key,
                                                                          size_t size)
{
  if (ocean == nullptr || size == 0) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(g_mesh_cache_mutex_);
  auto &by_key = g_ocean_gpu_ssbos_[ocean];
  auto it = by_key.find(key);
  if (it != by_key.end()) {
    InternalSSBOEntry &e = it->second;
    if (e.ssbo && e.capacity >= size) {
      return e.ssbo;
    }
    if (e.ssbo) {
      GPU_storagebuf_free(e.ssbo);
      e.ssbo = nullptr;
      e.capacity = 0;
    }
  }

  /* Use DYNAMIC by default since we tend to update these often. */
  std::string dbg_name = std::string("ocean_") + key;
  blender::gpu::StorageBuf *sb = GPU_storagebuf_create_ex(
      size, nullptr, GPU_USAGE_DYNAMIC, dbg_name.c_str());
  if (!sb) {
    return nullptr;
  }

  InternalSSBOEntry &dst = by_key[key];
  dst.ssbo = sb;
  dst.capacity = size;
  return sb;
}

blender::gpu::StorageBuf *MeshGPUCacheManager::ocean_internal_ssbo_get(struct Ocean *ocean,
                                                                       const std::string &key)
{
  if (ocean == nullptr) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(g_mesh_cache_mutex_);
  auto it_owner = g_ocean_gpu_ssbos_.find(ocean);
  if (it_owner == g_ocean_gpu_ssbos_.end()) {
    return nullptr;
  }
  auto it = it_owner->second.find(key);
  if (it == it_owner->second.end()) {
    return nullptr;
  }
  return it->second.ssbo;
}

void MeshGPUCacheManager::ocean_internal_ssbo_release(struct Ocean *ocean, const std::string &key)
{
  if (ocean == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mesh_cache_mutex_);
  auto it_owner = g_ocean_gpu_ssbos_.find(ocean);
  if (it_owner == g_ocean_gpu_ssbos_.end()) {
    return;
  }
  auto it = it_owner->second.find(key);
  if (it == it_owner->second.end()) {
    return;
  }
  if (it->second.ssbo) {
    GPU_storagebuf_free(it->second.ssbo);
  }
  it_owner->second.erase(it);
  if (it_owner->second.empty()) {
    g_ocean_gpu_ssbos_.erase(it_owner);
  }
}

void MeshGPUCacheManager::ocean_internal_ssbo_detach(struct Ocean *ocean, const std::string &key)
{
  if (ocean == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mesh_cache_mutex_);
  auto it_owner = g_ocean_gpu_ssbos_.find(ocean);
  if (it_owner == g_ocean_gpu_ssbos_.end()) {
    return;
  }
  auto it = it_owner->second.find(key);
  if (it == it_owner->second.end()) {
    return;
  }
  /* Do not free GPU buffer, just remove entry so ownership can be transferred. */
  it_owner->second.erase(it);
  if (it_owner->second.empty()) {
    g_ocean_gpu_ssbos_.erase(it_owner);
  }
}

void MeshGPUCacheManager::free_ocean_cache(struct Ocean *ocean)
{
  if (!ocean) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mesh_cache_mutex_);
  auto it_owner = g_ocean_gpu_ssbos_.find(ocean);
  if (it_owner == g_ocean_gpu_ssbos_.end()) {
    return;
  }
  for (auto &kv : it_owner->second) {
    if (kv.second.ssbo) {
      GPU_storagebuf_free(kv.second.ssbo);
    }
  }
  g_ocean_gpu_ssbos_.erase(it_owner);
}

void MeshGPUCacheManager::free_all_ocean_caches()
{
  std::lock_guard<std::mutex> lock(g_mesh_cache_mutex_);
  for (auto &kv_owner : g_ocean_gpu_ssbos_) {
    for (auto &kv : kv_owner.second) {
      if (kv.second.ssbo) {
        GPU_storagebuf_free(kv.second.ssbo);
      }
    }
  }
  g_ocean_gpu_ssbos_.clear();
}

/* ------------------------------------------------------------------------- */

std::unordered_map<const Mesh *, MeshGpuData> &MeshGPUCacheManager::mesh_cache()
{
#if defined(_DEBUG) || defined(DEBUG)
  /* Validate cache integrity: each entry should match its owner's session UID */
  for (const auto &[mesh, data] : g_mesh_data_cache_) {
    if (mesh != nullptr && data.session_uid != 0) {
      /* Assert fails if Mesh* pointer was reused for a different mesh */
      BLI_assert_msg(mesh->id.session_uid == data.session_uid,
                     "GPU cache pointer mismatch: Mesh* reused or dangling!");
    }
  }
#endif
  return g_mesh_data_cache_;
}

std::vector<MeshGpuData> &MeshGPUCacheManager::orphans()
{
  return g_mesh_data_orphans_;
}

std::mutex &MeshGPUCacheManager::mutex()
{
  return g_mesh_cache_mutex_;
}

void MeshGPUCacheManager::flush_orphans()
{
  extern void mesh_gpu_orphans_flush_impl();
  mesh_gpu_orphans_flush_impl();
}

void MeshGPUCacheManager::release_cpu_memory()
{
  std::lock_guard<std::mutex> lock(g_mesh_cache_mutex_);
  std::unordered_map<const Mesh *, MeshGpuData>().swap(g_mesh_data_cache_);
  std::vector<MeshGpuData>().swap(g_mesh_data_orphans_);
  /* Release owner map for Ocean (CPU side only; GPU should be freed via
   * free_all/free_all_ocean_caches). */
  std::unordered_map<const Ocean *, std::unordered_map<std::string, InternalSSBOEntry>>().swap(
      g_ocean_gpu_ssbos_);
}

}  // namespace bke
}  // namespace blender
