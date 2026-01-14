/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * GPU-accelerated Wave modifier manager (skeleton).
 *
 * This file reproduces the structure and API of `draw_displace.cc` but does
 * not implement the full functionality yet. It is intended as a scaffold so
 * the Wave GPU implementation can be filled incrementally. The design mirrors
 * Displace: caching per-mesh resources, SSBO/UBO helpers and a `dispatch_deform`
 * entry point.
 */

#include "draw_wave.hh"

#include "BLI_hash.h"
#include "BLI_math_matrix.h"
#include "BLI_map.hh"
#include "BLI_vector.hh"

#include "BKE_deform.hh"
#include "BKE_image.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_gpu.hh"

#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_storage_buffer.hh"

#include "MEM_guardedalloc.h"

#include "DEG_depsgraph_query.hh"

namespace blender {
namespace draw {

struct WaveManager::Impl {
  struct MeshModifierKey {
    Mesh *mesh;
    uint32_t modifier_uid;

    uint64_t hash() const { return (uint64_t(reinterpret_cast<uintptr_t>(mesh)) << 32) | uint64_t(modifier_uid); }
    bool operator==(const MeshModifierKey &other) const { return mesh == other.mesh && modifier_uid == other.modifier_uid; }
  };

  struct MeshStaticData {
    std::vector<float> vgroup_weights;
    std::vector<float3> tex_coords;
    int verts_num = 0;

    Object *deformed = nullptr;

    uint32_t last_verified_hash = 0;
    bool tex_is_byte = true;
    bool tex_is_float = false;
    int tex_channels = 4;
    uint32_t colorband_hash = 0;
    bool tex_metadata_cached = false;
  };

  Map<MeshModifierKey, MeshStaticData> static_map;
};

WaveManager &WaveManager::instance()
{
  static WaveManager manager;
  return manager;
}

WaveManager::WaveManager() : impl_(new Impl()) {}
WaveManager::~WaveManager() { delete impl_; }

void WaveManager::ensure_static_resources(const WaveModifierData *wmd, Object *deform_ob, Mesh *orig_mesh, uint32_t pipeline_hash)
{
  (void)wmd; (void)deform_ob; (void)orig_mesh; (void)pipeline_hash;
  /* TODO: implement resource extraction (vgroup, texcoords) similar to DisplaceManager::ensure_static_resources */
}

gpu::StorageBuf *WaveManager::dispatch_deform(const WaveModifierData *wmd,
                                              Depsgraph *depsgraph,
                                              Object *deformed_eval,
                                              MeshBatchCache *cache,
                                              gpu::StorageBuf *ssbo_in)
{
  (void)wmd; (void)depsgraph; (void)deformed_eval; (void)cache; (void)ssbo_in;
  /* TODO: implement shader assembly, UBO/SSBO/texture prep and dispatch. */
  return nullptr;
}

void WaveManager::free_resources_for_mesh(Mesh *mesh)
{
  if (!mesh) return;
  Vector<Impl::MeshModifierKey> keys_to_remove;
  for (const auto &item : impl_->static_map.items()) {
    if (item.key.mesh == mesh) {
      keys_to_remove.append(item.key);
    }
  }
}

void WaveManager::invalidate_all(Mesh *mesh)
{
  if (!mesh) return;
  bke::BKE_mesh_gpu_internal_resources_free_for_mesh(mesh);
}

void WaveManager::free_all()
{
  impl_->static_map.clear();
}

} // namespace draw
} // namespace blender
