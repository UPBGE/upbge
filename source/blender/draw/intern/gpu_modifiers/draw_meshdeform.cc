/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_meshdeform.hh"

#include <cmath>
#include <cstdio>

#include "BLI_array.hh"
#include "BLI_hash_c.hh"
#include "BLI_math_matrix_c.hh"
#include "BLI_vector.hh"

#include "BKE_deform.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_gpu.hh"
#include "BKE_mesh_wrapper.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"

#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

#include "draw_cache_extract.hh"

#include "GPU_compute.hh"
#include "GPU_shader.hh"

#include "draw_modifier_gpu_helpers.hh"

#include <vector>

namespace blender {
namespace draw {

struct blender::draw::MeshDeformSkinningManager::Impl {
  /* Composite key: (Mesh*, modifier UID) to support multiple Mesh Deform modifiers per mesh */
  struct MeshModifierKey {
    Mesh *mesh;
    uint32_t modifier_uid;

    uint64_t hash() const
    {
      return (uint64_t(reinterpret_cast<uintptr_t>(mesh)) << 32) | uint64_t(modifier_uid);
    }

    bool operator==(const MeshModifierKey &other) const
    {
      return mesh == other.mesh && modifier_uid == other.modifier_uid;
    }
  };

  struct MeshStaticData {
    std::vector<int> bindoffsets;
    std::vector<int> influence_vertices;
    std::vector<float> influence_weights;
    std::vector<float> bindcagecos; /* float3 per cage vertex, world-space bind coords */
    std::vector<float> vgroup_weights; /* per-vertex weight (0.0-1.0) */
    int verts_num = 0;
    int cage_verts_num = 0;

    Object *cage = nullptr;
    Object *deformed = nullptr;
    uint32_t last_verified_hash = 0;
  };

  Map<MeshModifierKey, MeshStaticData> static_map;
  bool debug_enabled = false;
};

/* Mesh deform compute shader (GPU port of MOD_meshdeform.cc) */
static const char *meshdeform_compute_src = R"GLSL(
void main() {
  uint v = gl_GlobalInvocationID.x;
  if (v >= deformed_positions.length()) {
    return;
  }

  vec4 co = input_positions[v];
  vec3 co_orig = co.xyz;

  /* Get per-vertex weight from vertex group (defaults to 1.0 if no vgroup) */
  float fac = 1.0;
  if (vgroup_weights.length() > 0 && int(v) < vgroup_weights.length()) {
    fac = vgroup_weights[v];
  }

  /* Apply invert vertex group flag */
  if (invert_vgroup != 0) {
    fac = 1.0 - fac;
  }

  if (fac <= 0.0) {
    deformed_positions[v] = co;
    return;
  }

  int start = bindoffsets[v];
  int end = bindoffsets[v + 1];

  vec3 delta = vec3(0.0);
  float totweight = 0.0;

  for (int i = start; i < end; i++) {
    int cage_v = influence_vertices[i];
    float weight = influence_weights[i];
    delta += weight * cage_deltas[cage_v];
    totweight += weight;
  }

  if (totweight > 0.0) {
    delta *= fac / totweight;
    /* Transform delta from bind space to deformed object space using mat3 part of mat4 */
    delta = mat3(cage_icagemat) * delta;
    co_orig += delta;
  }

  deformed_positions[v] = vec4(co_orig, 1.0);
}
)GLSL";

MeshDeformSkinningManager &MeshDeformSkinningManager::instance()
{
  static MeshDeformSkinningManager manager;
  return manager;
}

MeshDeformSkinningManager::MeshDeformSkinningManager() : impl_(new Impl()) {}
MeshDeformSkinningManager::~MeshDeformSkinningManager() {}

void MeshDeformSkinningManager::set_debug_enabled(bool enabled)
{
  impl_->debug_enabled = enabled;
}

uint32_t MeshDeformSkinningManager::compute_meshdeform_hash(const Mesh *mesh_orig,
                                                            const MeshDeformModifierData *mmd)
{
  if (!mesh_orig || !mmd) {
    return 0;
  }

  uint32_t hash = 0;
  hash = BLI_hash_int_2d(hash, mesh_orig->verts_num);

  if (mmd->object) {
    hash = BLI_hash_int_2d(hash, (int)(intptr_t)mmd->object);
  }

  hash = BLI_hash_int_2d(hash, mmd->verts_num);
  hash = BLI_hash_int_2d(hash, mmd->cage_verts_num);

  /* Hash bind data pointers to detect re-binding */
  hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(mmd->bindoffsets)));
  hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(mmd->bindinfluences)));
  hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(mmd->bindcagecos)));

  /* Hash vertex group name */
  if (mmd->defgrp_name[0] != '\0') {
    hash ^= BLI_hash_string(mmd->defgrp_name);
  }

  /* Hash deform verts pointer to detect vertex group changes */
  blender::Span<MDeformVert> dverts = mesh_orig->deform_verts();
  hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(dverts.data())));

  return hash;
}

void MeshDeformSkinningManager::ensure_static_resources(const MeshDeformModifierData *mmd,
                                                        Object *cage_ob,
                                                        Object *deformed_ob,
                                                        Mesh *orig_mesh,
                                                        uint32_t pipeline_hash)
{
  if (!orig_mesh || !cage_ob || !mmd) {
    return;
  }

  /* Dynamic bind is not supported on GPU yet */
  if (mmd->flag & MOD_MDEF_DYNAMIC_BIND) {
    return;
  }

  /* Need static bind data */
  if (!mmd->bindoffsets || !mmd->bindinfluences || !mmd->bindcagecos) {
    return;
  }

  Impl::MeshModifierKey key{orig_mesh, uint32_t(mmd->modifier.persistent_uid)};
  Impl::MeshStaticData &msd = impl_->static_map.lookup_or_add_default(key);

  const bool first_time = (msd.last_verified_hash == 0);
  const bool hash_changed = (pipeline_hash != msd.last_verified_hash);

  if (!first_time && !hash_changed) {
    return;
  }

  msd.last_verified_hash = pipeline_hash;
  msd.verts_num = mmd->verts_num;
  msd.cage_verts_num = mmd->cage_verts_num;
  msd.cage = cage_ob;
  msd.deformed = deformed_ob;

  /* Copy bind offsets */
  msd.bindoffsets.resize(mmd->verts_num + 1);
  memcpy(msd.bindoffsets.data(), mmd->bindoffsets, sizeof(int) * (mmd->verts_num + 1));

  /* Copy influence data */
  const int influences_num = mmd->bindoffsets[mmd->verts_num];
  msd.influence_vertices.resize(influences_num);
  msd.influence_weights.resize(influences_num);
  for (int i = 0; i < influences_num; i++) {
    msd.influence_vertices[i] = mmd->bindinfluences[i].vertex;
    msd.influence_weights[i] = mmd->bindinfluences[i].weight;
  }

  /* Copy bind cage coordinates (world-space bind coords) */
  msd.bindcagecos.resize(mmd->cage_verts_num * 3);
  memcpy(msd.bindcagecos.data(), mmd->bindcagecos, sizeof(float) * 3 * mmd->cage_verts_num);

  /* Extract vertex group weights from mesh */
  msd.vgroup_weights.clear();
  if (mmd->defgrp_name[0] != '\0') {
    const int defgrp_index = BKE_id_defgroup_name_index(&orig_mesh->id, mmd->defgrp_name);
    if (defgrp_index != -1) {
      blender::Span<MDeformVert> dverts = orig_mesh->deform_verts();
      if (!dverts.is_empty()) {
        msd.vgroup_weights.resize(orig_mesh->verts_num, 0.0f);
        for (int v = 0; v < orig_mesh->verts_num; ++v) {
          const MDeformVert &dvert = dverts[v];
          msd.vgroup_weights[v] = BKE_defvert_find_weight(&dvert, defgrp_index);
        }
      }
    }
  }
}

gpu::StorageBuf *MeshDeformSkinningManager::dispatch_deform(
    const MeshDeformModifierData *mmd,
    Depsgraph * /*depsgraph*/,
    Object *eval_cage,
    Object *deformed_eval,
    MeshBatchCache *cache,
    gpu::StorageBuf *ssbo_in)
{
  if (!mmd || !ssbo_in) {
    if (impl_->debug_enabled) {
      printf("MESHDEFORM_GPU: dispatch skipped (mmd=%p ssbo_in=%p)\n", mmd, ssbo_in);
    }
    return nullptr;
  }

  /* Dynamic bind is not supported on GPU yet */
  if (mmd->flag & MOD_MDEF_DYNAMIC_BIND) {
    if (impl_->debug_enabled) {
      printf("MESHDEFORM_GPU: dispatch skipped (dynamic bind not supported)\n");
    }
    return nullptr;
  }

  Mesh *mesh_owner = (cache && cache->mesh_owner) ? cache->mesh_owner : nullptr;
  if (!mesh_owner || !eval_cage) {
    if (impl_->debug_enabled) {
      printf("MESHDEFORM_GPU: dispatch skipped (mesh_owner=%p eval_cage=%p)\n",
             mesh_owner,
             eval_cage);
    }
    return nullptr;
  }

  Impl::MeshModifierKey key{mesh_owner, uint32_t(mmd->modifier.persistent_uid)};
  Impl::MeshStaticData *msd_ptr = impl_->static_map.lookup_ptr(key);
  if (!msd_ptr) {
    if (impl_->debug_enabled) {
      printf("MESHDEFORM_GPU: dispatch skipped (no static data for mesh %s)\n",
             (mesh_owner->id.name + 2));
    }
    return nullptr;
  }
  Impl::MeshStaticData &msd = *msd_ptr;

  /* Create unique buffer keys per modifier instance */
  const std::string key_prefix = "meshdeform_" + std::to_string(key.hash()) + "_";
  const std::string key_offsets = key_prefix + "offsets";
  const std::string key_influence_vertices = key_prefix + "influence_vertices";
  const std::string key_influence_weights = key_prefix + "influence_weights";
  const std::string key_cage_deltas = key_prefix + "cage_deltas";
  const std::string key_out = key_prefix + "output";

  /* Bind offsets SSBO */
  gpu::StorageBuf *ssbo_offsets = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_offsets);
  if (!ssbo_offsets) {
    const size_t size_offsets = msd.bindoffsets.size() * sizeof(int);
    ssbo_offsets = bke::BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, deformed_eval, key_offsets, size_offsets);
    if (ssbo_offsets) {
      GPU_storagebuf_update(ssbo_offsets, msd.bindoffsets.data());
    }
  }

  /* Influence vertices SSBO */
  gpu::StorageBuf *ssbo_influence_vertices = bke::BKE_mesh_gpu_internal_ssbo_get(
      mesh_owner, key_influence_vertices);
  if (!ssbo_influence_vertices) {
    const size_t size_iv = msd.influence_vertices.size() * sizeof(int);
    ssbo_influence_vertices = bke::BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, deformed_eval, key_influence_vertices, size_iv);
    if (ssbo_influence_vertices) {
      GPU_storagebuf_update(ssbo_influence_vertices, msd.influence_vertices.data());
    }
  }

  /* Influence weights SSBO */
  gpu::StorageBuf *ssbo_influence_weights = bke::BKE_mesh_gpu_internal_ssbo_get(
      mesh_owner, key_influence_weights);
  if (!ssbo_influence_weights) {
    const size_t size_iw = msd.influence_weights.size() * sizeof(float);
    ssbo_influence_weights = bke::BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, deformed_eval, key_influence_weights, size_iw);
    if (ssbo_influence_weights) {
      GPU_storagebuf_update(ssbo_influence_weights, msd.influence_weights.data());
    }
  }

  if (!ssbo_offsets || !ssbo_influence_vertices || !ssbo_influence_weights) {
    return nullptr;
  }

  /* Get current cage mesh and compute deltas */
  Mesh *cagemesh = BKE_modifier_get_evaluated_mesh_from_evaluated_object(eval_cage);
  if (!cagemesh) {
    if (impl_->debug_enabled) {
      printf("MESHDEFORM_GPU: dispatch skipped (no evaluated cage mesh)\n");
    }
    return nullptr;
  }

  if (cagemesh->verts_num != msd.cage_verts_num) {
    if (impl_->debug_enabled) {
      printf("MESHDEFORM_GPU: dispatch skipped (cage vertex count mismatch %d vs %d)\n",
             cagemesh->verts_num,
             msd.cage_verts_num);
    }
    return nullptr;
  }

  /* Compute cage transformation matrices (same as CPU MOD_meshdeform.cc) */
  float imat[4][4], cagemat[4][4], cmat[4][4], iobmat[4][4];
  invert_m4_m4(imat, eval_cage->object_to_world().ptr());
  mul_m4_m4m4(cagemat, imat, deformed_eval->object_to_world().ptr());
  mul_m4_m4m4(cmat, mmd->bindmat, cagemat);
  invert_m4_m4(iobmat, cmat);

  /* Compute cage deltas in bind space: bindmat * current_cage_local - bindcagecos */
  Array<float3> cage_local_pos(cagemesh->verts_num);
  BKE_mesh_wrapper_vert_coords_copy(cagemesh, cage_local_pos.as_mutable_span());

  std::vector<float> cage_deltas(msd.cage_verts_num * 3);
  const float (*bindcagecos)[3] = reinterpret_cast<const float (*)[3]>(msd.bindcagecos.data());
  for (int a = 0; a < msd.cage_verts_num; a++) {
    float cage_bind_pos[3];
    mul_v3_m4v3(cage_bind_pos, mmd->bindmat, cage_local_pos[a]);
    sub_v3_v3v3(&cage_deltas[a * 3], cage_bind_pos, bindcagecos[a]);
  }

  if (impl_->debug_enabled) {
    float max_delta = 0.0f;
    for (int a = 0; a < msd.cage_verts_num * 3; a++) {
      max_delta = std::max(max_delta, std::fabs(cage_deltas[a]));
    }
    printf("MESHDEFORM_GPU: cage=%s eval_cage_mat=[...] cage_verts=%d max_delta=%f\n",
           (eval_cage->id.name + 2),
           msd.cage_verts_num,
           max_delta);
  }

  /* Cage deltas SSBO (updated every frame) */
  gpu::StorageBuf *ssbo_cage_deltas = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner,
                                                                            key_cage_deltas);
  if (!ssbo_cage_deltas) {
    const size_t size_cd = cage_deltas.size() * sizeof(float);
    ssbo_cage_deltas = bke::BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, deformed_eval, key_cage_deltas, size_cd);
  }
  if (ssbo_cage_deltas) {
    GPU_storagebuf_update(ssbo_cage_deltas, cage_deltas.data());
  }

  if (!ssbo_cage_deltas) {
    if (impl_->debug_enabled) {
      printf("MESHDEFORM_GPU: dispatch skipped (failed to allocate cage deltas SSBO)\n");
    }
    return nullptr;
  }

  /* Create output SSBO */
  const size_t size_out = msd.verts_num * sizeof(float) * 4;
  gpu::StorageBuf *ssbo_out = bke::BKE_mesh_gpu_internal_ssbo_ensure(
      mesh_owner, deformed_eval, key_out, size_out);
  if (!ssbo_out) {
    if (impl_->debug_enabled) {
      printf("MESHDEFORM_GPU: dispatch skipped (failed to allocate output SSBO)\n");
    }
    return nullptr;
  }

  if (impl_->debug_enabled) {
    printf("MESHDEFORM_GPU: dispatch verts=%d cage_verts=%d\n", msd.verts_num, msd.cage_verts_num);
  }

  /* Create shader */
  const std::string shader_key = "meshdeform_deform";
  gpu::Shader *shader = bke::BKE_mesh_gpu_internal_shader_get(mesh_owner, shader_key);
  if (!shader) {
    using namespace gpu::shader;
    ShaderCreateInfo info("pyGPU_Shader");
    info.local_group_size(256, 1, 1);
    info.compute_source_generated = meshdeform_compute_src;

    /* Bindings */
    info.storage_buf(0, Qualifier::write, "vec4", "deformed_positions[]");
    info.storage_buf(1, Qualifier::read, "vec4", "input_positions[]");
    info.storage_buf(2, Qualifier::read, "int", "bindoffsets[]");
    info.storage_buf(3, Qualifier::read, "int", "influence_vertices[]");
    info.storage_buf(4, Qualifier::read, "float", "influence_weights[]");
    info.storage_buf(5, Qualifier::read, "vec3", "cage_deltas[]");
    info.storage_buf(6, Qualifier::read, "float", "vgroup_weights[]"); // Optional vertex group

    /* Push constants */
    info.push_constant(Type::float4x4_t, "cage_icagemat"); /* only 3x3 part is used */
    info.push_constant(Type::int_t, "invert_vgroup");

    shader = bke::BKE_mesh_gpu_internal_shader_ensure(mesh_owner, deformed_eval, shader_key, info);
  }

  if (!shader) {
    return nullptr;
  }

  /* Bind and dispatch */
  GPU_shader_bind(shader);

  GPU_storagebuf_bind(ssbo_out, 0);
  GPU_storagebuf_bind(ssbo_in, 1);
  GPU_storagebuf_bind(ssbo_offsets, 2);
  GPU_storagebuf_bind(ssbo_influence_vertices, 3);
  GPU_storagebuf_bind(ssbo_influence_weights, 4);
  GPU_storagebuf_bind(ssbo_cage_deltas, 5);

  /* Ensure vgroup SSBO using helper */
  gpu::StorageBuf *ssbo_vgroup = modifier_gpu_helpers::ensure_vgroup_ssbo(
      mesh_owner, deformed_eval, key_prefix + "vgroup_weights", msd.vgroup_weights, msd.verts_num);
  if (ssbo_vgroup) {
    GPU_storagebuf_bind(ssbo_vgroup, 6);
  }

  /* Set push constants */
  GPU_shader_uniform_mat4(shader, "cage_icagemat", iobmat);
  GPU_shader_uniform_1i(shader, "invert_vgroup", (mmd->flag & MOD_MDEF_INVERT_VGROUP) ? 1 : 0);

  const int group_size = 256;
  const int num_groups = (msd.verts_num + group_size - 1) / group_size;
  GPU_compute_dispatch(shader, num_groups, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  return ssbo_out;
}

void MeshDeformSkinningManager::free_resources_for_mesh(Mesh *mesh)
{
  if (!mesh) {
    return;
  }

  Vector<Impl::MeshModifierKey> keys_to_remove;
  for (const auto &item : impl_->static_map.items()) {
    if (item.key.mesh == mesh) {
      keys_to_remove.append(item.key);
    }
  }

  for (const Impl::MeshModifierKey &key : keys_to_remove) {
    impl_->static_map.remove(key);
  }
}

void MeshDeformSkinningManager::invalidate_all(Mesh *mesh)
{
  if (!mesh) {
    return;
  }
  /* Free all GPU resources (SSBOs + shaders) for this mesh */
  bke::BKE_mesh_gpu_internal_resources_free_for_mesh(mesh);
}

void MeshDeformSkinningManager::free_all()
{
  impl_->static_map.clear();
}

}  // namespace draw
}  // namespace blender
