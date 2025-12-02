/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_lattice_deform.hh"

#include "BLI_hash.h"
#include "BLI_map.hh"
#include "BLI_math_matrix.h"
#include "BLI_vector.hh"

#include "BKE_lattice.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_gpu.hh"
#include "BKE_modifier.hh"

#include "DNA_lattice_types.h"
#include "DNA_modifier_types.h"

#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_storage_buffer.hh"

#include "../gpu/intern/gpu_shader_create_info.hh"

#include "DRW_render.hh"
#include "draw_cache_impl.hh"

using namespace blender::draw;

struct blender::draw::LatticeSkinningManager::Impl {
  struct MeshStaticData {
    std::vector<float> control_points;  /* float3 per control point */
    int verts_num = 0;
    int lattice_u = 0, lattice_v = 0, lattice_w = 0;
    
    Object *lattice = nullptr;
    Object *deformed = nullptr;
    
    bool pending_gpu_setup = false;
    int gpu_setup_attempts = 0;
    uint32_t last_verified_hash = 0;
  };
  
  Map<Mesh *, MeshStaticData> static_map;
};

/* Trilinear interpolation compute shader (stub for now) */
static const char *lattice_compute_src = R"GLSL(
void main() {
  uint v = gl_GlobalInvocationID.x;
  if (v >= deformed_positions.length()) {
    return;
  }
  /* TODO: Implement trilinear interpolation */
  deformed_positions[v] = input_positions[v];  // Pass-through for now
}
)GLSL";

LatticeSkinningManager &LatticeSkinningManager::instance()
{
  static LatticeSkinningManager manager;
  return manager;
}

LatticeSkinningManager::LatticeSkinningManager() : impl_(new Impl()) {}
LatticeSkinningManager::~LatticeSkinningManager() {}

uint32_t LatticeSkinningManager::compute_lattice_hash(const Mesh *mesh, const Object *ob)
{
  if (!mesh || !ob) {
    return 0;
  }
  
  uint32_t hash = 0;
  hash = BLI_hash_int_2d(hash, mesh->verts_num);
  
  /* Find lattice modifier */
  for (ModifierData *md = static_cast<ModifierData *>(ob->modifiers.first); md; md = md->next) {
    if (md->type == eModifierType_Lattice) {
      LatticeModifierData *lmd = (LatticeModifierData *)md;
      if (lmd->object && lmd->object->data) {
        Lattice *lt = static_cast<Lattice *>(lmd->object->data);
        hash = BLI_hash_int_2d(hash, lt->pntsu);
        hash = BLI_hash_int_2d(hash, lt->pntsv);
        hash = BLI_hash_int_2d(hash, lt->pntsw);
      }
      break;
    }
  }
  
  return hash;
}

void LatticeSkinningManager::ensure_static_resources(Object *lattice_ob,
                                                   Object *deformed_ob,
                                                   Mesh *orig_mesh,
                                                   uint32_t pipeline_hash)
{
  if (!orig_mesh) {
    return;
  }
  
  Impl::MeshStaticData &msd = impl_->static_map.lookup_or_add_default(orig_mesh);
  
  const bool first_time = (msd.last_verified_hash == 0);
  const bool hash_changed = (pipeline_hash != msd.last_verified_hash);
  const bool gpu_invalidated = msd.pending_gpu_setup;
  
  if (!first_time && !hash_changed && !gpu_invalidated) {
    return;
  }
  
  msd.last_verified_hash = pipeline_hash;
  msd.verts_num = orig_mesh->verts_num;
  msd.lattice = lattice_ob;
  msd.deformed = deformed_ob;
  
  /* TODO: Extract lattice control points */
  
  if (first_time || hash_changed) {
    msd.pending_gpu_setup = true;
    msd.gpu_setup_attempts = 0;
  }
}

blender::gpu::StorageBuf *LatticeSkinningManager::dispatch_deform(Depsgraph * /*depsgraph*/,
                                                                Object * /*eval_lattice*/,
                                                                Object * /*deformed_eval*/,
                                                                MeshBatchCache *cache,
                                                                blender::gpu::StorageBuf *ssbo_in)
{
  Mesh *mesh_owner = (cache && cache->mesh_owner) ? cache->mesh_owner : nullptr;
  if (!mesh_owner) {
    return nullptr;
  }
  
  Impl::MeshStaticData *msd_ptr = impl_->static_map.lookup_ptr(mesh_owner);
  if (!msd_ptr) {
    return nullptr;
  }
  Impl::MeshStaticData &msd = *msd_ptr;
  
  const int MAX_ATTEMPTS = 3;
  if (msd.pending_gpu_setup) {
    if (msd.gpu_setup_attempts == 0) {
      msd.gpu_setup_attempts = 1;
      return nullptr;
    }
    if (msd.gpu_setup_attempts >= MAX_ATTEMPTS) {
      msd.pending_gpu_setup = false;
      msd.gpu_setup_attempts = 0;
      return nullptr;
    }
    msd.gpu_setup_attempts++;
  }
  
  MeshGpuInternalResources *ires = BKE_mesh_gpu_internal_resources_ensure(mesh_owner);
  if (!ires) {
    return nullptr;
  }
  
  if (msd.pending_gpu_setup) {
    msd.pending_gpu_setup = false;
    msd.gpu_setup_attempts = 0;
  }
  
  /* TODO: Implement full lattice deformation */
  /* For now, just pass through the input */
  return ssbo_in;
}

void LatticeSkinningManager::free_resources_for_mesh(Mesh *mesh)
{
  if (!mesh) {
    return;
  }
  impl_->static_map.remove(mesh);
}

void LatticeSkinningManager::invalidate_all(Mesh *mesh)
{
  if (!mesh) {
    return;
  }
  
  BKE_mesh_gpu_internal_resources_free_for_mesh(mesh);
  
  if (auto *msd_ptr = impl_->static_map.lookup_ptr(mesh)) {
    Impl::MeshStaticData &msd = *msd_ptr;
    msd.pending_gpu_setup = true;
    msd.gpu_setup_attempts = 0;
  }
}

void LatticeSkinningManager::free_all()
{
  impl_->static_map.clear();
}
