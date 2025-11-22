/* SPDX-FileCopyrightText:2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_armature_skinning.hh"

#include <map>

#include "BLI_map.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_threads.h"
#include "BLI_vector.hh"

#include "BKE_deform.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_gpu.hh"
#include "BKE_object.hh"

#include "GPU_capabilities.hh"
#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_storage_buffer.hh"

#include "../gpu/intern/gpu_shader_create_info.hh"

#include "DRW_render.hh"
#include "draw_cache_extract.hh"
#include "draw_cache_impl.hh"

#include "DNA_mesh_types.h"
#include <cstring>

#include "DEG_depsgraph_query.hh"

using namespace blender::draw;

struct blender::draw::ArmatureSkinningManager::Impl {
  int ref_count = 0;

  /* Static CPU-side buffers (kept per original mesh pointer key). */
  struct MeshStaticData {
    std::vector<int> in_indices;       /* size = verts *4 */
    std::vector<float> in_weights;     /* size = verts *4 */
    std::vector<float> rest_positions; /* float4 per vert (flattened) */
    int verts_num = 0;

    /* DO NOT store GPU pointers here; resources are owned by BKE_mesh_gpu. */
    Object *arm = nullptr;
    Object *deformed = nullptr;

    bool pending_gpu_setup = false;
    int gpu_setup_attempts = 0;
  };

  Map<Mesh *, MeshStaticData> static_map;

  struct ArmatureData {
    int refcount = 0;
    int bones = 0;
    /* Do not store StorageBuf* here. Use BKE_armature_gpu_internal_ssbo_* helpers. */
  };

  Map<Object *, ArmatureData> arm_map;
};

static const char *skin_compute_src = R"GLSL(
#ifndef CONTRIB_THRESHOLD
  #define CONTRIB_THRESHOLD 1e-4
#endif

vec4 skin_pos_object(int v_idx) {
  vec4 rest_pos_object = premat[0] * rest_positions[v_idx];
  vec4 acc = vec4(0.0);
  float tw =0.0;
  for (int i =0; i <4; ++i) {
    int b = in_idx[v_idx][i];
    float w = in_wgt[v_idx][i];
    if (w >0.0) {
      acc += (bone_pose_mat[b] * rest_pos_object) * w;
      tw += w;
    }
  }
  return (tw <= CONTRIB_THRESHOLD) ? rest_pos_object : (acc + rest_pos_object * (1.0 - tw));
}

void main() {
  uint v = gl_GlobalInvocationID.x;
  if (v >= skinned_vert_positions.length()) {
    return;
  }
  skinned_vert_positions[v] = skin_pos_object(int(v));
}
)GLSL";

ArmatureSkinningManager &ArmatureSkinningManager::instance()
{
  static ArmatureSkinningManager manager;
  return manager;
}

ArmatureSkinningManager::ArmatureSkinningManager() : impl_(new Impl()) {}
ArmatureSkinningManager::~ArmatureSkinningManager() {}

void ArmatureSkinningManager::ensure_static_resources(Object *arm_ob,
                                                      Object *deformed_ob,
                                                      Mesh *orig_mesh)
{
  (void)deformed_ob;
  if (!orig_mesh) {
    return;
  }

  Impl::MeshStaticData &msd = impl_->static_map.lookup_or_add_default(orig_mesh);

  if (msd.verts_num == orig_mesh->verts_num) {
    /* Already prepared */
    return;
  }

  const int verts_num = orig_mesh->verts_num;
  msd.verts_num = verts_num;
  msd.in_indices.clear();
  msd.in_weights.clear();
  msd.rest_positions.clear();

  msd.in_indices.resize(verts_num * 4, 0);
  msd.in_weights.resize(verts_num * 4, 0.0f);
  msd.rest_positions.resize(verts_num * 4, 0.0f); /* float4 */

  /* Build group name -> bone index map from armature pose. */
  Map<std::string, int> bone_name_to_index;
  if (arm_ob && arm_ob->pose) {
    int idx = 0;
    for (bPoseChannel *pchan = (bPoseChannel *)arm_ob->pose->chanbase.first; pchan;
         pchan = pchan->next)
    {
      if (!(pchan->bone->flag & BONE_NO_DEFORM)) {
        bone_name_to_index.add_new(pchan->name, idx++);
      }
    }
  }

  /* Vertex group names/order from original mesh */
  std::vector<std::string> group_names;
  const ListBase *defbase = BKE_id_defgroup_list_get(&orig_mesh->id);
  if (defbase) {
    for (const bDeformGroup *dg = (const bDeformGroup *)defbase->first; dg; dg = dg->next) {
      group_names.push_back(dg->name);
    }
  }

  /* Fill influences from deform verts if present */
  blender::Span<MDeformVert> dverts = orig_mesh->deform_verts();
  constexpr float kContribThreshold = 1e-4f;

  for (int v = 0; v < verts_num; ++v) {
    const MDeformVert &dvert = dverts[v];
    std::map<int, float> bone_weight_map;
    for (int j = 0; j < dvert.totweight; ++j) {
      const int def_nr = dvert.dw[j].def_nr;
      if (def_nr >= 0 && def_nr < (int)group_names.size()) {
        const std::string &group_name = group_names[def_nr];
        if (auto *it = bone_name_to_index.lookup_ptr(group_name)) {
          bone_weight_map[*it] += dvert.dw[j].weight;
        }
      }
    }

    struct Influence {
      int bone_idx;
      float weight;
    };
    std::vector<Influence> influences;
    float total_raw = 0.0f;
    for (const auto &kv : bone_weight_map) {
      influences.push_back({kv.first, kv.second});
      total_raw += kv.second;
    }

    if (total_raw <= kContribThreshold || influences.empty()) {
      for (int j = 0; j < 4; ++j) {
        msd.in_indices[v * 4 + j] = 0;
        msd.in_weights[v * 4 + j] = 0.0f;
      }
    }
    else {
      std::sort(influences.begin(), influences.end(), [](const Influence &a, const Influence &b) {
        return a.weight > b.weight;
      });
      float total = 0.0f;
      for (const auto &inf : influences) {
        total += inf.weight;
      }
      if (total > 0.0f) {
        for (auto &inf : influences) {
          inf.weight /= total;
        }
      }
      for (int j = 0; j < 4; ++j) {
        if (j < (int)influences.size()) {
          msd.in_indices[v * 4 + j] = influences[j].bone_idx;
          msd.in_weights[v * 4 + j] = influences[j].weight;
        }
        else {
          msd.in_indices[v * 4 + j] = 0;
          msd.in_weights[v * 4 + j] = 0.0f;
        }
      }
    }
  }

  /* Rest positions (float4) from orig_mesh vert positions */
  blender::Span<blender::float3> vert_positions = orig_mesh->vert_positions();
  for (int i = 0; i < verts_num; ++i) {
    const blender::float3 &p = vert_positions[i];
    msd.rest_positions[i * 4 + 0] = p.x;
    msd.rest_positions[i * 4 + 1] = p.y;
    msd.rest_positions[i * 4 + 2] = p.z;
    msd.rest_positions[i * 4 + 3] = 1.0f;
  }

  /* Remember armature/deformed pointers so dispatch can compute premat/postmat. */
  msd.arm = arm_ob;
  msd.deformed = deformed_ob;

  /* Mark as pending GPU setup: the draw cache must be populated while `is_using_gpu_deform==1` to
   * produce float4 vertex positions. We cannot call `draw_cache_populate` here (no DrwContext),
   * so we set a pending flag and the first GL pass will set `is_using_gpu_deform` and return,
   * allowing the draw manager to populate the cache on the next frame. */
  msd.pending_gpu_setup = true;
  msd.gpu_setup_attempts = 0;

  /* GPU SSBO creation/upload will be deferred until in GL context (update_per_frame or dispatch).
   */
}

bool ArmatureSkinningManager::dispatch_skinning(Depsgraph *depsgraph,
                                                Object *armature,
                                                Object *deformed_eval,
                                                MeshBatchCache *cache,
                                                blender::gpu::VertBuf *vbo_pos,
                                                blender::gpu::VertBuf *vbo_nor)
{
  (void)vbo_pos;
  (void)vbo_nor;
  (void)deformed_eval;

  Mesh *mesh_owner = (cache && cache->mesh_owner) ? cache->mesh_owner : nullptr;
  if (!mesh_owner) {
    return false;
  }
  Impl::MeshStaticData *msd_ptr = impl_->static_map.lookup_ptr(mesh_owner);
  if (!msd_ptr) {
    return false;
  }
  Impl::MeshStaticData &msd = *msd_ptr;

  const int MAX_ATTEMPTS = 3;
  if (msd.pending_gpu_setup) {
    if (msd.gpu_setup_attempts == 0) {
      mesh_owner->is_running_gpu_animation_playback = 1;
      msd.gpu_setup_attempts = 1;
      return false;
    }
    if (msd.gpu_setup_attempts >= MAX_ATTEMPTS) {
      mesh_owner->is_running_gpu_animation_playback = 0;
      msd.pending_gpu_setup = false;
      msd.gpu_setup_attempts = 0;
      return false;
    }
  }

  MeshGpuInternalResources *ires = BKE_mesh_gpu_internal_resources_ensure(mesh_owner);
  if (!ires) {
    return false;
  }

  const std::string key_in_idx = "armature_in_idx";
  const std::string key_in_wgt = "armature_in_wgt";
  const std::string key_bone_pose = "armature_bone_pose";
  const std::string key_rest_pos = "armature_rest_pos";
  const std::string key_skinned_pos = "armature_skinned_pos";
  const std::string key_premat = "armature_premat";
  const std::string key_postmat = "armature_postmat";

  float premat[4][4], postmat[4][4], obinv[4][4];
  copy_m4_m4(premat, deformed_eval->object_to_world().ptr());
  invert_m4_m4(obinv, deformed_eval->object_to_world().ptr());
  mul_m4_m4m4(postmat, obinv, armature->object_to_world().ptr());
  invert_m4_m4(premat, postmat);

  /* ensure/upload per-mesh SSBOs (use GPU_storagebuf_update directly) */
  blender::gpu::StorageBuf *ssbo_in_idx = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_in_idx);
  if (!ssbo_in_idx) {
    ssbo_in_idx = BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, key_in_idx, sizeof(int) * msd.verts_num * 4);
    if (!ssbo_in_idx) {
      return false;
    }
    GPU_storagebuf_update(ssbo_in_idx, msd.in_indices.data());
  }

  blender::gpu::StorageBuf *ssbo_in_wgt = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_in_wgt);
  if (!ssbo_in_wgt) {
    ssbo_in_wgt = BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, key_in_wgt, sizeof(float) * msd.verts_num * 4);
    if (!ssbo_in_wgt) {
      return false;
    }
    GPU_storagebuf_update(ssbo_in_wgt, msd.in_weights.data());
  }

  blender::gpu::StorageBuf *ssbo_rest_pos = BKE_mesh_gpu_internal_ssbo_get(mesh_owner,
                                                                           key_rest_pos);
  if (!ssbo_rest_pos) {
    ssbo_rest_pos = BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, key_rest_pos, sizeof(float) * msd.verts_num * 4);
    if (!ssbo_rest_pos) {
      return false;
    }
    GPU_storagebuf_update(ssbo_rest_pos, msd.rest_positions.data());
  }

  blender::gpu::StorageBuf *ssbo_skinned_pos = BKE_mesh_gpu_internal_ssbo_get(mesh_owner,
                                                                              key_skinned_pos);
  if (!ssbo_skinned_pos) {
    ssbo_skinned_pos = BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, key_skinned_pos, sizeof(float) * msd.verts_num * 4);
    if (!ssbo_skinned_pos) {
      return false;
    }
  }

  blender::gpu::StorageBuf *ssbo_premat = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_premat);
  if (!ssbo_premat) {
    ssbo_premat = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_premat, sizeof(float) * 16);
    if (!ssbo_premat) {
      return false;
    }
  }
  GPU_storagebuf_update(ssbo_premat, &premat[0][0]);

  /* armature bone matrices */
  blender::gpu::StorageBuf *ssbo_bone_mat = nullptr;
  Impl::ArmatureData *ad_ref_ptr = nullptr;
  if (msd.arm) {
    Impl::ArmatureData &ad_ref = impl_->arm_map.lookup_or_add_default(msd.arm);
    ad_ref_ptr = &ad_ref;

    if (ad_ref.bones == 0) {
      int bc = 0;
      for (bPoseChannel *pchan = (bPoseChannel *)msd.arm->pose->chanbase.first; pchan;
           pchan = pchan->next)
      {
        if (!(pchan->bone->flag & BONE_NO_DEFORM)) {
          bc++;
        }
      }
      ad_ref.bones = bc;
    }
    if (ad_ref.bones > 0) {
      ssbo_bone_mat = BKE_armature_gpu_internal_ssbo_get(msd.arm, key_bone_pose);
      if (!ssbo_bone_mat) {
        ssbo_bone_mat = BKE_armature_gpu_internal_ssbo_ensure(
            msd.arm, key_bone_pose, sizeof(float) * 16 * ad_ref.bones);
      }
      if (ssbo_bone_mat) {
        std::vector<float> mats;
        mats.resize(size_t(ad_ref.bones) * 16);
        int bi = 0;
        for (bPoseChannel *pchan = (bPoseChannel *)msd.arm->pose->chanbase.first; pchan;
             pchan = pchan->next)
        {
          if (pchan->bone->flag & BONE_NO_DEFORM) {
            continue;
          }
          memcpy(&mats[bi * 16], pchan->chan_mat, sizeof(float) * 16);
          bi++;
        }
        GPU_storagebuf_update(ssbo_bone_mat, mats.data());
      }
    }
  }

  /* create/ensure compute shader and dispatch */
  using namespace blender::gpu::shader;
  ShaderCreateInfo info("pyGPU_Shader");
  info.local_group_size(256, 1, 1);
  info.compute_source_generated = skin_compute_src;
  info.storage_buf(0, Qualifier::write, "vec4", "skinned_vert_positions[]");
  info.storage_buf(1, Qualifier::read, "ivec4", "in_idx[]");
  info.storage_buf(2, Qualifier::read, "vec4", "in_wgt[]");
  info.storage_buf(3, Qualifier::read, "mat4", "bone_pose_mat[]");
  info.storage_buf(4, Qualifier::read, "mat4", "premat[]");
  info.storage_buf(5, Qualifier::read, "vec4", "rest_positions[]");
  const std::string shader_key = "armature_skinning_compute";
  blender::gpu::Shader *compute_sh = BKE_mesh_gpu_internal_shader_ensure(
      mesh_owner, shader_key, info);
  if (!compute_sh) {
    return false;
  }

  GPU_shader_bind(compute_sh);
  GPU_storagebuf_bind(ssbo_skinned_pos, 0);
  GPU_storagebuf_bind(ssbo_in_idx, 1);
  GPU_storagebuf_bind(ssbo_in_wgt, 2);
  if (ssbo_bone_mat) {
    GPU_storagebuf_bind(ssbo_bone_mat, 3);
  }
  GPU_storagebuf_bind(ssbo_premat, 4);
  GPU_storagebuf_bind(ssbo_rest_pos, 5);

  const int group_size = 256;
  int num_groups = (msd.verts_num + group_size - 1) / group_size;
  GPU_compute_dispatch(compute_sh, num_groups, 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  /* scatter to corners */
  if (deformed_eval && cache && vbo_pos && vbo_nor) {
    blender::gpu::StorageBuf *ssbo_postmat = BKE_mesh_gpu_internal_ssbo_get(mesh_owner,
                                                                            key_postmat);
    if (!ssbo_postmat) {
      ssbo_postmat = BKE_mesh_gpu_internal_ssbo_ensure(
          mesh_owner, key_postmat, sizeof(float) * 16);
      if (!ssbo_postmat) {
        return false;
      }
    }
    GPU_storagebuf_update(ssbo_postmat, &postmat[0][0]);

    std::vector<blender::bke::GpuMeshComputeBinding> caller_bindings;
    caller_bindings.reserve(4);

    {
      blender::bke::GpuMeshComputeBinding b = {};
      b.binding = 0;
      b.qualifiers = blender::gpu::shader::Qualifier::read_write;
      b.type_name = "vec4";
      b.bind_name = "positions_out[]";
      b.buffer = vbo_pos;
      caller_bindings.push_back(b);
    }
    {
      blender::bke::GpuMeshComputeBinding b = {};
      b.binding = 1;
      b.qualifiers = blender::gpu::shader::Qualifier::write;
      b.type_name = "uint";
      b.bind_name = "normals_out[]";
      b.buffer = vbo_nor;
      caller_bindings.push_back(b);
    }
    {
      blender::bke::GpuMeshComputeBinding b = {};
      b.binding = 2;
      b.qualifiers = blender::gpu::shader::Qualifier::read;
      b.type_name = "vec4";
      b.bind_name = "positions_in[]";
      b.buffer = ssbo_skinned_pos;
      caller_bindings.push_back(b);
    }
    {
      blender::bke::GpuMeshComputeBinding b = {};
      b.binding = 3;
      b.qualifiers = blender::gpu::shader::Qualifier::read;
      b.type_name = "mat4";
      b.bind_name = "transform_mat[]";
      b.buffer = ssbo_postmat;
      caller_bindings.push_back(b);
    }

    auto post_bind_fn = [](blender::gpu::Shader * /* sh */) {};
    auto config_fn = [](blender::gpu::shader::ShaderCreateInfo & /* info */) {};

    Mesh *mesh_eval = (Mesh *)deformed_eval->data;
    if (!mesh_eval) {
      return false;
    }

    if (msd.pending_gpu_setup) {
      msd.pending_gpu_setup = false;
      msd.gpu_setup_attempts = 0;
    }

    BKE_mesh_gpu_scatter_to_corners(depsgraph,
                                    deformed_eval,
                                    caller_bindings,
                                    config_fn,
                                    post_bind_fn,
                                    mesh_eval->corners_num);
  }

  return true;
}

void ArmatureSkinningManager::free_resources_for_mesh(Mesh *mesh)
{
  if (!mesh) {
    return;
  }

  if (auto *msd_ptr = impl_->static_map.lookup_ptr(mesh)) {
    Impl::MeshStaticData &msd = *msd_ptr;

    /* Decrement armature refcount and free arm data if unused. */
    if (msd.arm) {
      if (auto *ad_ptr = impl_->arm_map.lookup_ptr(msd.arm)) {
        Impl::ArmatureData &ad = *ad_ptr;
        ad.refcount -= 1;
        if (ad.refcount <= 0) {
          /* Release per-armature GPU resources that we created (bone matrices SSBO). */
          const std::string key_bone_pose = "armature_bone_pose";
          BKE_armature_gpu_internal_ssbo_release(msd.arm, key_bone_pose);

          /* If in the future you add more per-arm keys, release them here too:
           * BKE_armature_gpu_internal_ssbo_release(msd.arm, "<other_key>");
           */

          impl_->arm_map.remove(msd.arm);
        }
      }
    }

    /* Remove CPU-side static data for this mesh. GPU resources owned by the mesh
     * are freed elsewhere via BKE_mesh_gpu_free_for_mesh(mesh). */
    impl_->static_map.remove(mesh);
  }
}

void ArmatureSkinningManager::free_all()
{
  /* Clear CPU-side maps. Per-mesh GPU resources are freed by BKE_mesh_gpu_free_all_caches()
   * or per-mesh frees elsewhere. */
  impl_->static_map.clear();
  impl_->arm_map.clear();
}
