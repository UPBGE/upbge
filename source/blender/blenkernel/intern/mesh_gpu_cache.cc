/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "mesh_gpu_cache.hh"

#include "BKE_mesh.hh"
#include "BKE_mesh_gpu.hh"

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
}

void MeshGPUCacheManager::free_all_armature_caches()
{
  BKE_armature_gpu_internal_free_all_armature_caches();
}

void MeshGPUCacheManager::free_for_mesh(struct Mesh *mesh)
{
  BKE_mesh_gpu_free_for_mesh(mesh);
}

MeshGpuInternalResources *MeshGPUCacheManager::mesh_internal_resources_ensure(struct Mesh *mesh)
{
  return BKE_mesh_gpu_internal_resources_ensure(mesh);
}

blender::gpu::StorageBuf *MeshGPUCacheManager::armature_internal_ssbo_ensure(
    struct Object *arm, const std::string &key, size_t size)
{
  return BKE_armature_gpu_internal_ssbo_ensure(arm, key, size);
}

blender::gpu::StorageBuf *MeshGPUCacheManager::armature_internal_ssbo_get(struct Object *arm,
                                                                          const std::string &key)
{
  return BKE_armature_gpu_internal_ssbo_get(arm, key);
}

void MeshGPUCacheManager::armature_internal_ssbo_release(struct Object *arm,
                                                         const std::string &key)
{
  BKE_armature_gpu_internal_ssbo_release(arm, key);
}

std::unordered_map<const Mesh *, MeshGpuData> &MeshGPUCacheManager::mesh_cache()
{
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

std::unordered_map<const Object *, blender::bke::MeshGpuInternalResources> &MeshGPUCacheManager::
    armature_resources()
{
  return g_armature_gpu_resources_;
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
  std::unordered_map<const Object *, blender::bke::MeshGpuInternalResources>().swap(
      g_armature_gpu_resources_);
}

}  // namespace bke
}  // namespace blender
