/* SPDX-FileCopyrightText:2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_armature_skinning.hh"

#include <cstring>
#include <map>

#include "BLI_hash.h"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_quaternion.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_threads.h"
#include "BLI_vector.hh"

#include "BKE_deform.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_gpu.hh"
#include "BKE_object.hh"

#include "DNA_armature_types.h"
#include "DNA_modifier_types.h"
#include "DNA_vec_types.h"

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

#include "DEG_depsgraph_query.hh"

using namespace blender::draw;

/* Dual Quaternion structure matching Blender's CPU format */
struct GPUDualQuat {
  float quat[4];      /* Rotation quaternion [w,x,y,z] */
  float trans[4];     /* Translation dual part [w,x,y,z] */
  float scale[4][4];  /* Scale matrix */
  float scale_weight; /* Weight for scale blending */
  float _pad[3];
};
static_assert(sizeof(GPUDualQuat) % 16 == 0, "GPUDualQuat must be 16-byte aligned");

struct blender::draw::ArmatureSkinningManager::Impl {
  int ref_count = 0;

  /* Static CPU-side buffers (kept per original mesh pointer key). */
  struct MeshStaticData {
    std::vector<int> in_influence_offsets; /* size = verts + 1, offset into in_indices */
    std::vector<int> in_indices;           /* size = total_influences (variable per vertex) */
    std::vector<float> in_weights;         /* size = total_influences (variable per vertex) */
    std::vector<float> rest_positions;     /* float4 per vert (flattened) */
    std::vector<float> vgroup_weights;     /* per-vertex weight (0.0-1.0) for modifier filter */
    int verts_num = 0;

    /* DO NOT store GPU pointers here; resources are owned by BKE_mesh_gpu. */
    Object *arm = nullptr;
    Object *deformed = nullptr;

    bool pending_gpu_setup = false;
    int gpu_setup_attempts = 0;
    /* Cache last computed hash to detect Armature changes */
    uint32_t last_verified_hash = 0;
  };

  Map<Mesh *, MeshStaticData> static_map;

  struct ArmatureData {
    int refcount = 0;
    int bones = 0;
    /* Do not store StorageBuf* here. Use BKE_armature_gpu_internal_ssbo_* helpers. */
  };

  Map<Object *, ArmatureData> arm_map;
};

/* Linear Blend Skinning shader */
static const char *skin_compute_lbs_src = R"GLSL(
#ifndef CONTRIB_THRESHOLD
  #define CONTRIB_THRESHOLD 1e-4
#endif

vec4 skin_pos_object(int v_idx) {
  vec4 rest_pos_object = premat[0] * rest_positions[v_idx];

  /* Get influence range for this vertex */
  int start_idx = in_offsets[v_idx];
  int end_idx = in_offsets[v_idx + 1];
  int influence_count = end_idx - start_idx;

  /* No influences = rest pose */
  if (influence_count == 0) {
    return rest_pos_object;
  }

  vec4 acc = vec4(0.0);
  float tw = 0.0;

  /* Process all influences for this vertex (no limit!) */
  for (int i = 0; i < influence_count; ++i) {
    int idx = start_idx + i;
    int b = in_idx[idx];
    float w = in_wgt[idx];

    if (w > 0.0) {
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

  /* Get modifier vertex group weight (filter - like Lattice) */
  float modifier_weight = 1.0;
  if (vgroup_weights.length() > 0 && v < vgroup_weights.length()) {
    modifier_weight = vgroup_weights[v];
  }

  /* Early exit if weight is negligible */
  if (modifier_weight < 1e-6) {
    skinned_vert_positions[v] = postmat[0] * (premat[0] * rest_positions[v]);
    return;
  }

  vec4 skinned = skin_pos_object(int(v));
  vec4 rest = premat[0] * rest_positions[v];

  /* Blend between rest and skinned based on modifier weight */
  skinned_vert_positions[v] = postmat[0] * mix(rest, skinned, modifier_weight);
}
)GLSL";

/* Dual Quaternion Skinning shader */
static const char *skin_compute_dqs_src = R"GLSL(
#ifndef CONTRIB_THRESHOLD
  #define CONTRIB_THRESHOLD 1e-4
#endif

vec4 quat_multiply(vec4 q1, vec4 q2) {
  return vec4(
    q1.w * q2.x + q1.x * q2.w + q1.y * q2.z - q1.z * q2.y,
    q1.w * q2.y - q1.x * q2.z + q1.y * q2.w + q1.z * q2.x,
    q1.w * q2.z + q1.x * q2.y - q1.y * q2.x + q1.z * q2.w,
    q1.w * q2.w - q1.x * q2.x - q1.y * q2.y - q1.z * q2.z
  );
}

vec4 skin_pos_object(int v_idx) {
  /* Transform rest position to armature space first */
  vec3 co = (premat[0] * rest_positions[v_idx]).xyz;

  /* Get influence range for this vertex */
  int start_idx = in_offsets[v_idx];
  int end_idx = in_offsets[v_idx + 1];
  int influence_count = end_idx - start_idx;

  /* No influences = rest pose */
  if (influence_count == 0) {
    return vec4(co, 1.0);
  }

  /* Accumulated dual quaternion components */
  vec4 quat_sum = vec4(0.0);
  vec4 trans_sum = vec4(0.0);
  mat4 scale_sum = mat4(0.0);
  float scale_weight_sum = 0.0;

  float total_weight = 0.0;
  bool first_bone = true;

  /* Process all influences for this vertex (no limit!) */
  for (int i = 0; i < influence_count; ++i) {
    int idx = start_idx + i;
    int b = in_idx[idx];
    float w = in_wgt[idx];

    if (w > 0.0 && b >= 0) {
      /* Read bone dual quaternion components stored as [w,x,y,z] */
      vec4 bone_quat_wxyz = bone_dq_quat[b];
      vec4 bone_trans_wxyz = bone_dq_trans[b];

      /* Reorder from [w,x,y,z] to [x,y,z,w] for shader processing */
      vec4 bone_quat = vec4(bone_quat_wxyz.y, bone_quat_wxyz.z, bone_quat_wxyz.w, bone_quat_wxyz.x);
      vec4 bone_trans = vec4(bone_trans_wxyz.y, bone_trans_wxyz.z, bone_trans_wxyz.w, bone_trans_wxyz.x);

      mat4 bone_scale = bone_dq_scale[b];
      float bone_scale_weight = bone_dq_scale_weight[b];

      /* Flip quaternion if dot product is negative (shortest path) */
      bool flip = false;
      if (!first_bone && dot(quat_sum, bone_quat) < 0.0) {
        flip = true;
        w = -w;
      }

      /* Accumulate rotation and translation */
      quat_sum += w * bone_quat;
      trans_sum += w * bone_trans;

      /* Accumulate scale if present */
      if (bone_scale_weight > 0.0) {
        float scale_w = flip ? -w : w;
        scale_sum += scale_w * bone_scale;
        scale_weight_sum += abs(w);
      }

      total_weight += abs(w);
      first_bone = false;
    }
  }

  if (total_weight <= CONTRIB_THRESHOLD) {
    return vec4(co, 1.0);
  }

  /* Normalize accumulated dual quaternion */
  float scale = 1.0 / total_weight;
  quat_sum *= scale;
  trans_sum *= scale;

  if (scale_weight_sum > 0.0) {
    float addweight = total_weight - scale_weight_sum;
    if (addweight > 0.0) {
      scale_sum[0][0] += addweight;
      scale_sum[1][1] += addweight;
      scale_sum[2][2] += addweight;
      scale_sum[3][3] += addweight;
    }
    scale_sum *= scale;
  }

  /* Transform point using dual quaternion (now in [x,y,z,w] format) */
  float w = quat_sum.w, x = quat_sum.x, y = quat_sum.y, z = quat_sum.z;
  float t0 = trans_sum.w, t1 = trans_sum.x, t2 = trans_sum.y, t3 = trans_sum.z;

  /* Build rotation matrix from quaternion */
  mat3 M;
  M[0][0] = w * w + x * x - y * y - z * z;
  M[1][0] = 2.0 * (x * y - w * z);
  M[2][0] = 2.0 * (x * z + w * y);

  M[0][1] = 2.0 * (x * y + w * z);
  M[1][1] = w * w + y * y - x * x - z * z;
  M[2][1] = 2.0 * (y * z - w * x);

  M[0][2] = 2.0 * (x * z - w * y);
  M[1][2] = 2.0 * (y * z + w * x);
  M[2][2] = w * w + z * z - x * x - y * y;

  float len2 = dot(quat_sum, quat_sum);
  if (len2 > 0.0) {
    len2 = 1.0 / len2;
  }

  /* Extract translation from dual quaternion */
  vec3 t;
  t[0] = 2.0 * (-t0 * x + w * t1 - t2 * z + y * t3);
  t[1] = 2.0 * (-t0 * y + t1 * z - x * t3 + w * t2);
  t[2] = 2.0 * (-t0 * z + x * t2 + w * t3 - t1 * y);

  /* Apply transformation */
  vec3 result = co;

  /* Apply scale first if present */
  if (scale_weight_sum > 0.0) {
    result = (scale_sum * vec4(result, 1.0)).xyz;
  }

  /* Apply rotation and translation */
  result = M * result;
  result[0] = (result[0] + t[0]) * len2;
  result[1] = (result[1] + t[1]) * len2;
  result[2] = (result[2] + t[2]) * len2;

  return vec4(result, 1.0);
}

void main() {
  uint v = gl_GlobalInvocationID.x;
  if (v >= skinned_vert_positions.length()) {
    return;
  }

  /* Get modifier vertex group weight (filter - like Lattice) */
  float modifier_weight = 1.0;
  if (vgroup_weights.length() > 0 && v < vgroup_weights.length()) {
    modifier_weight = vgroup_weights[v];
  }

  /* Early exit if weight is negligible */
  if (modifier_weight < 1e-6) {
    skinned_vert_positions[v] = postmat[0] * (premat[0] * rest_positions[v]);
    return;
  }

  vec4 skinned = skin_pos_object(int(v));
  vec4 rest = premat[0] * rest_positions[v];

  /* Blend between rest and skinned based on modifier weight */
  skinned_vert_positions[v] = postmat[0] * mix(rest, skinned, modifier_weight);
}
)GLSL";

ArmatureSkinningManager &ArmatureSkinningManager::instance()
{
  static ArmatureSkinningManager manager;
  return manager;
}

ArmatureSkinningManager::ArmatureSkinningManager() : impl_(new Impl()) {}
ArmatureSkinningManager::~ArmatureSkinningManager() {}

/* Compute a hash of the Armature deformation state to detect changes */
uint32_t ArmatureSkinningManager::compute_armature_hash(const Mesh *mesh_orig,
                                                        const ArmatureModifierData *amd)
{
  if (!mesh_orig || !amd) {
    return 0;
  }

  uint32_t hash = 0;

  /* Hash number of vertices */
  hash = BLI_hash_int_2d(hash, mesh_orig->verts_num);

  /* Hash armature object pointer */
  if (amd->object) {
    hash = BLI_hash_int_2d(hash, (int)(intptr_t)amd->object);
  }

  /* Hash DQS mode (affects shader variant) */
  /* Don't hash use_dqs - we want to keep the possibility
   * to switch fast between dqs and lbs shaders without invalidating
   * all armature/mesh resources. */
  //const bool use_dqs = (amd->deformflag & ARM_DEF_QUATERNION) != 0;
  //hash = BLI_hash_int_2d(hash, use_dqs ? 1 : 0);

  /* Hash vertex group name (if specified) - like Lattice modifier */
  if (amd->defgrp_name[0] != '\0') {
    hash = BLI_hash_string(amd->defgrp_name);
  }

  return hash;
}

void ArmatureSkinningManager::ensure_static_resources(const ArmatureModifierData *amd,
                                                      Object *arm_ob,
                                                      Object *deformed_ob,
                                                      Mesh *orig_mesh,
                                                      uint32_t pipeline_hash)
{
  (void)deformed_ob;
  if (!orig_mesh || !amd) {
    return;
  }

  Impl::MeshStaticData &msd = impl_->static_map.lookup_or_add_default(orig_mesh);

  /* Check if recalculation is needed by comparing pipeline hash.
   * The hash is computed by
   * GPUModifierPipeline and includes ALL Armature state
   * (vertex count, armature pointer, DQS
   * mode, vertex groups, bone count).
   *
   * We recalculate CPU influences when:
   * 1. First
   * time (last_verified_hash == 0)
   * 2. Hash changed (pipeline_hash != last_verified_hash)

   * * 3. GPU resources were invalidated (pending_gpu_setup == true) */
  const bool first_time = (msd.last_verified_hash == 0);
  const bool hash_changed = (pipeline_hash != msd.last_verified_hash);
  const bool gpu_invalidated = msd.pending_gpu_setup;

  if (!first_time && !hash_changed && !gpu_invalidated) {
    return;  // No changes detected, reuse cached influences
  }

  /* Recalculate influences (triggered by hash change or GPU invalidation) */
  if (0) {
    printf(
        "Recalculating Armature influences for mesh '%s' (first=%d, hash_changed=%d, "
        "gpu_inv=%d)\n",
        (orig_mesh->id.name + 2),
        first_time,
        hash_changed,
        gpu_invalidated);
  }

  /* Update hash cache */
  msd.last_verified_hash = pipeline_hash;

  const int verts_num = orig_mesh->verts_num;
  msd.verts_num = verts_num;
  msd.in_influence_offsets.clear();
  msd.in_indices.clear();
  msd.in_weights.clear();
  msd.rest_positions.clear();

  msd.in_influence_offsets.resize(verts_num + 1, 0); /* +1 for end offset */
  msd.rest_positions.resize(verts_num * 4, 0.0f);    /* float4 */

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
  constexpr float kContribThreshold = 0.0001f;

  /* First pass: count total influences and build offsets */
  int total_influences = 0;
  for (int v = 0; v < verts_num; ++v) {
    msd.in_influence_offsets[v] = total_influences;

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

    /* Count significant influences */
    for (const auto &kv : bone_weight_map) {
      if (kv.second > kContribThreshold) {
        total_influences++;
      }
    }
  }
  msd.in_influence_offsets[verts_num] = total_influences; /* End offset */

  /* Allocate arrays for all influences */
  msd.in_indices.resize(total_influences);
  msd.in_weights.resize(total_influences);

  /* Second pass: fill influences (no limit!) */
  int influence_idx = 0;
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

    /* Collect and sort influences */
    struct Influence {
      int bone_idx;
      float weight;
    };
    std::vector<Influence> influences;
    influences.reserve(bone_weight_map.size());

    float total_weight = 0.0f;
    for (const auto &kv : bone_weight_map) {
      if (kv.second > kContribThreshold) {
        influences.push_back({kv.first, kv.second});
        total_weight += kv.second;
      }
    }

    /* Sort by weight (descending) */
    std::sort(influences.begin(), influences.end(), [](const Influence &a, const Influence &b) {
      return a.weight > b.weight;
    });

    /* Normalize weights */
    if (total_weight > kContribThreshold) {
      const float inv_total = 1.0f / total_weight;
      for (auto &inf : influences) {
        inf.weight *= inv_total;
      }
    }

    /* Store all influences (no 16-bone limit!) */
    for (const auto &inf : influences) {
      msd.in_indices[influence_idx] = inf.bone_idx;
      msd.in_weights[influence_idx] = inf.weight;
      influence_idx++;
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

  /* Mark as pending GPU setup if this is a new calculation (not just a GPU invalidation retry).
   * If gpu_invalidated was true, pending_gpu_setup is already true, so no need to reset it. */
  if (first_time || hash_changed) {
    msd.pending_gpu_setup = true;
    msd.gpu_setup_attempts = 0;
  }

  /* Extract vertex group weights from mesh (modifier vertex group filter - like Lattice) */
  msd.vgroup_weights.clear();
  if (amd->defgrp_name[0] != '\0') {
    /* Find vertex group index in mesh */
    const int defgrp_index = BKE_id_defgroup_name_index(&orig_mesh->id, amd->defgrp_name);
    if (defgrp_index != -1) {
      /* Extract per-vertex weights */
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

  /* GPU SSBO creation/upload will be deferred until in GL context (update_per_frame or dispatch).
   */
}

blender::gpu::StorageBuf *ArmatureSkinningManager::dispatch_skinning(
    const ArmatureModifierData *amd,
    Depsgraph * /*depsgraph*/,
    Object *eval_armature,
    Object *deformed_eval,
    MeshBatchCache *cache,
    blender::gpu::StorageBuf *ssbo_in)
{
  if (!amd) {
    return nullptr;
  }

  Mesh *mesh_owner = (cache && cache->mesh_owner) ? cache->mesh_owner : nullptr;
  if (!mesh_owner) {
    return nullptr;
  }
  Impl::MeshStaticData *msd_ptr = impl_->static_map.lookup_ptr(mesh_owner);
  if (!msd_ptr) {
    return nullptr;
  }
  Impl::MeshStaticData &msd = *msd_ptr;

  /* Check if dual quaternion skinning is enabled (now using amd directly!) */
  const bool use_dual_quaternions = (amd->deformflag & ARM_DEF_QUATERNION) != 0;

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
    /* If we reach here, gpu_setup_attempts is between 1 and MAX_ATTEMPTS-1.
     * Increment and continue to attempt GPU setup. */
    msd.gpu_setup_attempts++;
  }

  MeshGpuInternalResources *ires = BKE_mesh_gpu_internal_resources_ensure(mesh_owner);
  if (!ires) {
    return nullptr;
  }

  /* GPU setup successful! Clear pending flag. */
  if (msd.pending_gpu_setup) {
    msd.pending_gpu_setup = false;
    msd.gpu_setup_attempts = 0;
  }

  const std::string key_in_idx = "armature_in_idx";
  const std::string key_in_wgt = "armature_in_wgt";
  const std::string key_in_offsets = "armature_in_offsets";
  const std::string key_bone_pose = "armature_bone_pose";
  const std::string key_rest_pos = "armature_rest_pos";
  const std::string key_skinned_pos = "armature_skinned_pos";
  const std::string key_premat = "armature_premat";
  const std::string key_postmat = "armature_postmat";

  /* Compute premat and postmat for coordinate space conversion */
  float premat[4][4], postmat[4][4], obinv[4][4];
  invert_m4_m4(obinv, deformed_eval->object_to_world().ptr());
  copy_m4_m4(premat, deformed_eval->object_to_world().ptr());
  mul_m4_m4m4(postmat, obinv, eval_armature->object_to_world().ptr());
  invert_m4_m4(premat, postmat);

  /* ensure/upload per-mesh SSBOs (use GPU_storagebuf_update directly) */
  blender::gpu::StorageBuf *ssbo_in_offsets = BKE_mesh_gpu_internal_ssbo_get(mesh_owner,
                                                                             key_in_offsets);
  if (!ssbo_in_offsets) {
    ssbo_in_offsets = BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, key_in_offsets, sizeof(int) * (msd.verts_num + 1));
    if (!ssbo_in_offsets) {
      return nullptr;
    }
    GPU_storagebuf_update(ssbo_in_offsets, msd.in_influence_offsets.data());
  }

  blender::gpu::StorageBuf *ssbo_in_idx = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_in_idx);
  if (!ssbo_in_idx) {
    ssbo_in_idx = BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, key_in_idx, sizeof(int) * msd.in_indices.size());
    if (!ssbo_in_idx) {
      return nullptr;
    }
    GPU_storagebuf_update(ssbo_in_idx, msd.in_indices.data());
  }

  blender::gpu::StorageBuf *ssbo_in_wgt = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_in_wgt);
  if (!ssbo_in_wgt) {
    ssbo_in_wgt = BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, key_in_wgt, sizeof(float) * msd.in_weights.size());
    if (!ssbo_in_wgt) {
      return nullptr;
    }
    GPU_storagebuf_update(ssbo_in_wgt, msd.in_weights.data());
  }

  /* Vertex group weights SSBO (modifier filter - like Lattice) */
  const std::string key_vgroup = "armature_vgroup_weights";
  blender::gpu::StorageBuf *ssbo_vgroup = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_vgroup);
  
  /* Only create/upload if vertex group weights exist */
  if (!msd.vgroup_weights.empty()) {
    if (!ssbo_vgroup) {
      const size_t size_vgroup = msd.vgroup_weights.size() * sizeof(float);
      ssbo_vgroup = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_vgroup, size_vgroup);
      if (ssbo_vgroup) {
        GPU_storagebuf_update(ssbo_vgroup, msd.vgroup_weights.data());
      }
    }
  }
  else {
    /* No vertex group: create empty dummy buffer (length=0 triggers default weight=1.0 in shader) */
    if (!ssbo_vgroup) {
      ssbo_vgroup = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_vgroup, sizeof(float));
      if (ssbo_vgroup) {
        float dummy = 1.0f;  /* Unused, but set to 1.0 for safety */
        GPU_storagebuf_update(ssbo_vgroup, &dummy);
      }
    }
  }


  blender::gpu::StorageBuf *ssbo_rest_pos = BKE_mesh_gpu_internal_ssbo_get(mesh_owner,
                                                                           key_rest_pos);
  if (!ssbo_rest_pos) {
    ssbo_rest_pos = BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, key_rest_pos, sizeof(float) * msd.verts_num * 4);
    if (!ssbo_rest_pos) {
      return nullptr;
    }
    GPU_storagebuf_update(ssbo_rest_pos, msd.rest_positions.data());
  }

  blender::gpu::StorageBuf *ssbo_skinned_pos = BKE_mesh_gpu_internal_ssbo_get(mesh_owner,
                                                                              key_skinned_pos);
  if (!ssbo_skinned_pos) {
    ssbo_skinned_pos = BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, key_skinned_pos, sizeof(float) * msd.verts_num * 4);
    if (!ssbo_skinned_pos) {
      return nullptr;
    }
  }

  blender::gpu::StorageBuf *ssbo_premat = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_premat);
  if (!ssbo_premat) {
    ssbo_premat = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_premat, sizeof(float) * 16);
    if (!ssbo_premat) {
      return nullptr;
    }
  }
  GPU_storagebuf_update(ssbo_premat, &premat[0][0]);

  blender::gpu::StorageBuf *ssbo_postmat = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_postmat);
  if (!ssbo_postmat) {
    ssbo_postmat = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_postmat, sizeof(float) * 16);
    if (!ssbo_postmat) {
      return nullptr;
    }
  }
  GPU_storagebuf_update(ssbo_postmat, &postmat[0][0]);

  /* armature bone matrices or dual quaternions */
  blender::gpu::StorageBuf *ssbo_bone_mat = nullptr;
  blender::gpu::StorageBuf *ssbo_bone_dq_quat = nullptr;
  blender::gpu::StorageBuf *ssbo_bone_dq_trans = nullptr;
  blender::gpu::StorageBuf *ssbo_bone_dq_scale = nullptr;
  blender::gpu::StorageBuf *ssbo_bone_dq_scale_weight = nullptr;

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
      if (use_dual_quaternions) {
        /* Upload Dual Quaternions for Preserve Volume */
        const std::string key_dq_quat = "armature_dq_quat";
        const std::string key_dq_trans = "armature_dq_trans";
        const std::string key_dq_scale = "armature_dq_scale";
        const std::string key_dq_scale_weight = "armature_dq_scale_weight";

        ssbo_bone_dq_quat = BKE_armature_gpu_internal_ssbo_get(msd.arm, key_dq_quat);
        if (!ssbo_bone_dq_quat) {
          ssbo_bone_dq_quat = BKE_armature_gpu_internal_ssbo_ensure(
              msd.arm, key_dq_quat, sizeof(float) * 4 * ad_ref.bones);
        }

        ssbo_bone_dq_trans = BKE_armature_gpu_internal_ssbo_get(msd.arm, key_dq_trans);
        if (!ssbo_bone_dq_trans) {
          ssbo_bone_dq_trans = BKE_armature_gpu_internal_ssbo_ensure(
              msd.arm, key_dq_trans, sizeof(float) * 4 * ad_ref.bones);
        }

        ssbo_bone_dq_scale = BKE_armature_gpu_internal_ssbo_get(msd.arm, key_dq_scale);
        if (!ssbo_bone_dq_scale) {
          ssbo_bone_dq_scale = BKE_armature_gpu_internal_ssbo_ensure(
              msd.arm, key_dq_scale, sizeof(float) * 16 * ad_ref.bones);
        }

        ssbo_bone_dq_scale_weight = BKE_armature_gpu_internal_ssbo_get(msd.arm,
                                                                       key_dq_scale_weight);
        if (!ssbo_bone_dq_scale_weight) {
          ssbo_bone_dq_scale_weight = BKE_armature_gpu_internal_ssbo_ensure(
              msd.arm, key_dq_scale_weight, sizeof(float) * ad_ref.bones);
        }

        /* ALWAYS update dual quaternions every frame (not just on creation) */
        if (ssbo_bone_dq_quat && ssbo_bone_dq_trans && ssbo_bone_dq_scale &&
            ssbo_bone_dq_scale_weight)
        {
          std::vector<float> quats(size_t(ad_ref.bones) * 4);
          std::vector<float> trans(size_t(ad_ref.bones) * 4);
          std::vector<float> scales(size_t(ad_ref.bones) * 16);
          std::vector<float> scale_weights(size_t(ad_ref.bones));

          int bi = 0;
          for (bPoseChannel *pchan = (bPoseChannel *)msd.arm->pose->chanbase.first; pchan;
               pchan = pchan->next)
          {
            if (pchan->bone->flag & BONE_NO_DEFORM) {
              continue;
            }

            float imat[4][4];
            if (pchan->bone) {
              invert_m4_m4(imat, pchan->bone->arm_mat);
              mul_m4_m4m4(pchan->chan_mat, pchan->pose_mat, imat);
              mat4_to_dquat(
                  &pchan->runtime.deform_dual_quat, pchan->bone->arm_mat, pchan->chan_mat);
            }

            /* Use the pre-computed dual quaternion from runtime (same as CPU skinning) */
            const DualQuat &dq = pchan->runtime.deform_dual_quat;

            /* Copy quat [w,x,y,z] - already in correct space */
            quats[bi * 4 + 0] = dq.quat[0];
            quats[bi * 4 + 1] = dq.quat[1];
            quats[bi * 4 + 2] = dq.quat[2];
            quats[bi * 4 + 3] = dq.quat[3];

            /* Copy trans [w,x,y,z] - already in correct space */
            trans[bi * 4 + 0] = dq.trans[0];
            trans[bi * 4 + 1] = dq.trans[1];
            trans[bi * 4 + 2] = dq.trans[2];
            trans[bi * 4 + 3] = dq.trans[3];

            /* Copy scale matrix 4x4 */
            memcpy(&scales[bi * 16], dq.scale, sizeof(float) * 16);

            /* Copy scale_weight */
            scale_weights[bi] = dq.scale_weight;

            bi++;
          }

          /* Update GPU buffers every frame */
          GPU_storagebuf_update(ssbo_bone_dq_quat, quats.data());
          GPU_storagebuf_update(ssbo_bone_dq_trans, trans.data());
          GPU_storagebuf_update(ssbo_bone_dq_scale, scales.data());
          GPU_storagebuf_update(ssbo_bone_dq_scale_weight, scale_weights.data());
        }
      }
      else {
        /* Upload standard matrices for LBS */
        ssbo_bone_mat = BKE_armature_gpu_internal_ssbo_get(msd.arm, key_bone_pose);
        if (!ssbo_bone_mat) {
          ssbo_bone_mat = BKE_armature_gpu_internal_ssbo_ensure(
              msd.arm, key_bone_pose, sizeof(float) * 16 * ad_ref.bones);
        }

        /* ALWAYS update bone matrices every frame (not just on creation) */
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
          /* Update GPU buffer every frame */
          GPU_storagebuf_update(ssbo_bone_mat, mats.data());
        }
      }
    }
  }

  /* create/ensure compute shader and dispatch */
  using namespace blender::gpu::shader;
  ShaderCreateInfo info("pyGPU_Shader");
  info.local_group_size(256, 1, 1);

  /* Select shader source based on skinning mode */
  if (use_dual_quaternions) {
    info.compute_source_generated = skin_compute_dqs_src;
    info.storage_buf(0, Qualifier::write, "vec4", "skinned_vert_positions[]");
    info.storage_buf(1, Qualifier::read, "int", "in_offsets[]");
    info.storage_buf(2, Qualifier::read, "int", "in_idx[]");
    info.storage_buf(3, Qualifier::read, "float", "in_wgt[]");
    info.storage_buf(4, Qualifier::read, "vec4", "bone_dq_quat[]");
    info.storage_buf(5, Qualifier::read, "vec4", "bone_dq_trans[]");
    info.storage_buf(6, Qualifier::read, "mat4", "bone_dq_scale[]");
    info.storage_buf(7, Qualifier::read, "float", "bone_dq_scale_weight[]");
    info.storage_buf(8, Qualifier::read, "mat4", "premat[]");
    info.storage_buf(9, Qualifier::read, "vec4", "rest_positions[]");
    info.storage_buf(10, Qualifier::read, "mat4", "postmat[]");
    info.storage_buf(11, Qualifier::read, "float", "vgroup_weights[]");  // Modifier filter
  }
  else {
    info.compute_source_generated = skin_compute_lbs_src;
    info.storage_buf(0, Qualifier::write, "vec4", "skinned_vert_positions[]");
    info.storage_buf(1, Qualifier::read, "int", "in_offsets[]");
    info.storage_buf(2, Qualifier::read, "int", "in_idx[]");
    info.storage_buf(3, Qualifier::read, "float", "in_wgt[]");
    info.storage_buf(4, Qualifier::read, "mat4", "bone_pose_mat[]");
    info.storage_buf(5, Qualifier::read, "mat4", "premat[]");
    info.storage_buf(6, Qualifier::read, "vec4", "rest_positions[]");
    info.storage_buf(7, Qualifier::read, "mat4", "postmat[]");
    info.storage_buf(8, Qualifier::read, "float", "vgroup_weights[]");  // Modifier filter
  }

  const std::string shader_key = use_dual_quaternions ? "armature_skinning_dqs" :
                                                        "armature_skinning_lbs";

  blender::gpu::Shader *compute_sh = BKE_mesh_gpu_internal_shader_ensure(
      mesh_owner, shader_key, info);
  if (!compute_sh) {
    return nullptr;
  }

  blender::gpu::StorageBuf *pos_to_bind = ssbo_in ? ssbo_in : ssbo_skinned_pos;

  GPU_shader_bind(compute_sh);

  if (use_dual_quaternions) {
    /* Bind DQS buffers */
    GPU_storagebuf_bind(pos_to_bind, 0);
    GPU_storagebuf_bind(ssbo_in_offsets, 1);
    GPU_storagebuf_bind(ssbo_in_idx, 2);
    GPU_storagebuf_bind(ssbo_in_wgt, 3);
    if (ssbo_bone_dq_quat) {
      GPU_storagebuf_bind(ssbo_bone_dq_quat, 4);
    }
    if (ssbo_bone_dq_trans) {
      GPU_storagebuf_bind(ssbo_bone_dq_trans, 5);
    }
    if (ssbo_bone_dq_scale) {
      GPU_storagebuf_bind(ssbo_bone_dq_scale, 6);
    }
    if (ssbo_bone_dq_scale_weight) {
      GPU_storagebuf_bind(ssbo_bone_dq_scale_weight, 7);
    }
    GPU_storagebuf_bind(ssbo_premat, 8);
    GPU_storagebuf_bind(ssbo_rest_pos, 9);
    GPU_storagebuf_bind(ssbo_postmat, 10);
    if (ssbo_vgroup) {
      GPU_storagebuf_bind(ssbo_vgroup, 11);
    }
  }
  else {
    /* Bind LBS buffers */
    GPU_storagebuf_bind(pos_to_bind, 0);
    GPU_storagebuf_bind(ssbo_in_offsets, 1);
    GPU_storagebuf_bind(ssbo_in_idx, 2);
    GPU_storagebuf_bind(ssbo_in_wgt, 3);
    if (ssbo_bone_mat) {
      GPU_storagebuf_bind(ssbo_bone_mat, 4);
    }
    GPU_storagebuf_bind(ssbo_premat, 5);
    GPU_storagebuf_bind(ssbo_rest_pos, 6);
    GPU_storagebuf_bind(ssbo_postmat, 7);
    if (ssbo_vgroup) {
      GPU_storagebuf_bind(ssbo_vgroup, 8);
    }
  }

  const int group_size = 256;
  int num_groups = (msd.verts_num + group_size - 1) / group_size;
  GPU_compute_dispatch(compute_sh, num_groups, 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  /* Return the SSBO containing the skinned positions. Caller will perform scatter if needed. */
  return pos_to_bind;
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
          /* Release per-armature GPU resources that we created */
          const std::string key_bone_pose = "armature_bone_pose";
          BKE_armature_gpu_internal_ssbo_release(msd.arm, key_bone_pose);

          /* Release dual quaternion resources if they exist */
          const std::string key_dq_quat = "armature_dq_quat";
          const std::string key_dq_trans = "armature_dq_trans";
          const std::string key_dq_scale = "armature_dq_scale";
          const std::string key_dq_scale_weight = "armature_dq_scale_weight";

          BKE_armature_gpu_internal_ssbo_release(msd.arm, key_dq_quat);
          BKE_armature_gpu_internal_ssbo_release(msd.arm, key_dq_trans);
          BKE_armature_gpu_internal_ssbo_release(msd.arm, key_dq_scale);
          BKE_armature_gpu_internal_ssbo_release(msd.arm, key_dq_scale_weight);

          impl_->arm_map.remove(msd.arm);
        }
      }
    }

    /* Remove CPU-side static data for this mesh. GPU resources owned by the mesh
     * are freed elsewhere via BKE_mesh_gpu_free_for_mesh(mesh). */
    impl_->static_map.remove(mesh);
  }
}

void ArmatureSkinningManager::invalidate_all(Mesh *mesh)
{
  if (!mesh) {
    return;
  }

  /* 1. Free all GPU resources (SSBOs + shaders) for this mesh */
  BKE_mesh_gpu_internal_resources_free_for_mesh(mesh);

  /* 2. Mark CPU data as "GPU not initialized" to trigger recreation */
  if (auto *msd_ptr = impl_->static_map.lookup_ptr(mesh)) {
    Impl::MeshStaticData &msd = *msd_ptr;
    msd.pending_gpu_setup = true;
    msd.gpu_setup_attempts = 0;
    /* Keep CPU data (influences, rest_positions, etc.) for fast recreation */
  }
}

void ArmatureSkinningManager::free_all()
{
  /* Clear CPU-side maps. Per-mesh GPU resources are freed by BKE_mesh_gpu_free_all_caches()
   * or per-mesh frees elsewhere. */
  impl_->static_map.clear();
  impl_->arm_map.clear();
}
