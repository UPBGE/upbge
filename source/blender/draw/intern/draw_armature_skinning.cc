/* SPDX-FileCopyrightText:2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_armature_skinning.hh"

#include <map>

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
#include <cstring>

#include "DEG_depsgraph_query.hh"

using namespace blender::draw;

/* Dual Quaternion structure matching Blender's CPU format */
struct GPUDualQuat {
  float quat[4];    /* Rotation quaternion [w,x,y,z] */
  float trans[4];   /* Translation dual part [w,x,y,z] */
  float scale[4][4]; /* Scale matrix */
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
    int verts_num = 0;

    /* DO NOT store GPU pointers here; resources are owned by BKE_mesh_gpu. */
    Object *arm = nullptr;
    Object *deformed = nullptr;

    bool pending_gpu_setup = false;
    int gpu_setup_attempts = 0;
    /* Note: use_dual_quaternions is checked dynamically each frame, not cached here */
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
  skinned_vert_positions[v] = skin_pos_object(int(v));
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
  msd.in_influence_offsets.clear();
  msd.in_indices.clear();
  msd.in_weights.clear();
  msd.rest_positions.clear();

  msd.in_influence_offsets.resize(verts_num + 1, 0); /* +1 for end offset */
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
                                                blender::gpu::VertBuf *vbo_nor,
                                                blender::gpu::StorageBuf *ssbo_in,
                                                bool do_scatter)
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

  /* Check if dual quaternion skinning is enabled (check every frame for UI changes) */
  bool use_dual_quaternions = false;
  if (msd.deformed && msd.deformed->modifiers.first) {
    LISTBASE_FOREACH (ModifierData *, md, &msd.deformed->modifiers) {
      if (md->type == eModifierType_Armature) {
        ArmatureModifierData *amd = (ArmatureModifierData *)md;
        use_dual_quaternions = (amd->deformflag & ARM_DEF_QUATERNION) != 0;
        break;
      }
    }
  }

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
  const std::string key_in_offsets = "armature_in_offsets";
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
  blender::gpu::StorageBuf *ssbo_in_offsets = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_in_offsets);
  if (!ssbo_in_offsets) {
    ssbo_in_offsets = BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, key_in_offsets, sizeof(int) * (msd.verts_num + 1));
    if (!ssbo_in_offsets) {
      return false;
    }
    GPU_storagebuf_update(ssbo_in_offsets, msd.in_influence_offsets.data());
  }

  blender::gpu::StorageBuf *ssbo_in_idx = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_in_idx);
  if (!ssbo_in_idx) {
    ssbo_in_idx = BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, key_in_idx, sizeof(int) * msd.in_indices.size());
    if (!ssbo_in_idx) {
      return false;
    }
    GPU_storagebuf_update(ssbo_in_idx, msd.in_indices.data());
  }

  blender::gpu::StorageBuf *ssbo_in_wgt = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_in_wgt);
  if (!ssbo_in_wgt) {
    ssbo_in_wgt = BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, key_in_wgt, sizeof(float) * msd.in_weights.size());
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

        ssbo_bone_dq_scale_weight = BKE_armature_gpu_internal_ssbo_get(msd.arm, key_dq_scale_weight);
        if (!ssbo_bone_dq_scale_weight) {
          ssbo_bone_dq_scale_weight = BKE_armature_gpu_internal_ssbo_ensure(
              msd.arm, key_dq_scale_weight, sizeof(float) * ad_ref.bones);
        }

        /* ALWAYS update dual quaternions every frame (not just on creation) */
        if (ssbo_bone_dq_quat && ssbo_bone_dq_trans && ssbo_bone_dq_scale && ssbo_bone_dq_scale_weight) {
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
  }

  const std::string shader_key = use_dual_quaternions ?
      "armature_skinning_dqs" : "armature_skinning_lbs";

  blender::gpu::Shader *compute_sh = BKE_mesh_gpu_internal_shader_ensure(
      mesh_owner, shader_key, info);
  if (!compute_sh) {
    return false;
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
  }

  const int group_size = 256;
  int num_groups = (msd.verts_num + group_size - 1) / group_size;
  GPU_compute_dispatch(compute_sh, num_groups, 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  if (do_scatter)
  {
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
        b.buffer = pos_to_bind;
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

void ArmatureSkinningManager::free_all()
{
  /* Clear CPU-side maps. Per-mesh GPU resources are freed by BKE_mesh_gpu_free_all_caches()
   * or per-mesh frees elsewhere. */
  impl_->static_map.clear();
  impl_->arm_map.clear();
}
