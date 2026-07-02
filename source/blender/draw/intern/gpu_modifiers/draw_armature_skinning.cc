/* SPDX-FileCopyrightText:2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_armature_skinning.hh"

#include <map>

#include "BLI_hash_c.hh"
#include "BLI_math_matrix_c.hh"
#include "BLI_math_rotation_c.hh"

#include "BKE_armature.hh"
#include "BKE_deform.hh"
#include "BKE_mesh_gpu.hh"

#include "DNA_modifier_types.h"

#include "draw_cache_extract.hh"
#include "DRW_render.hh"

#include "GPU_compute.hh"

#include "draw_modifier_gpu_helpers.hh"

namespace blender {
namespace draw {

struct blender::draw::ArmatureSkinningManager::Impl {
  int ref_count = 0;

  /* Composite key: (Mesh*, modifier UID) to support multiple armatures per mesh */
  struct MeshModifierKey {
    Mesh *mesh;
    uint32_t modifier_uid; /* Use persistent_uid like all other modifiers */

    uint64_t hash() const
    {
      return (uint64_t(reinterpret_cast<uintptr_t>(mesh)) << 32) | uint64_t(modifier_uid);
    }

    bool operator==(const MeshModifierKey &other) const
    {
      return mesh == other.mesh && modifier_uid == other.modifier_uid;
    }
  };

  /* Static CPU-side buffers (kept per (mesh, modifier) key). */
  struct MeshStaticData {
    std::vector<int> in_influence_offsets; /* size = verts + 1, offset into in_indices */
    std::vector<int> in_indices;           /* size = total_influences (variable per vertex) */
    std::vector<float> in_weights;         /* size = total_influences (variable per vertex) */
    std::vector<float> vgroup_weights;     /* per-vertex weight (0.0-1.0) for modifier filter */
    int verts_num = 0;
    int bones = 0; /* Number of deformable bones in armature (cached to avoid recounting) */

    /* B-Bone support */
    std::vector<int> bone_segment_counts;      /* segments per bone (1 for simple, N for B-Bone) */
    std::vector<int> bone_segment_offsets_lbs; /* cumulative offset for LBS (segments+2) */
    std::vector<int> bone_segment_offsets_dqs; /* cumulative offset for DQS (segments+1) */
    int total_segments_lbs = 0;                /* total slots for LBS */
    int total_segments_dqs = 0;                /* total slots for DQS */
    uint32_t bbone_config_hash = 0;            /* hash to detect B-Bone config changes */

    Object *arm = nullptr;
    Object *deformed = nullptr;
    /* Cache last computed hash to detect Armature changes */
    uint32_t last_verified_hash = 0;
  };

  Map<MeshModifierKey, MeshStaticData> static_map;
};

/* Linear Blend Skinning shader with B-Bone support */
static const char *skin_compute_lbs_src = R"GLSL(
#ifndef CONTRIB_THRESHOLD
  #define CONTRIB_THRESHOLD 1e-4
#endif

/* Get bone matrix with B-Bone segment interpolation
 * Reproduces CPU logic from b_bone_deform() → find_bbone_segment_index_straight()
 *
 * IMPORTANT: co must be in ARMATURE SPACE (already transformed by premat)
 *
 * CPU reference (find_bbone_segment_index_straight):
 *   const Mat4 *mats = pchan->runtime.bbone_deform_mats;
 *   const float (*mat)[4] = mats[0].mat;
 *   const float y = mat[0][1]*co[0] + mat[1][1]*co[1] + mat[2][1]*co[2] + mat[3][1];
 *   BKE_pchan_bbone_deform_clamp_segment_index(bone, y / bone->length, &index, &blend);
 *
 * CPU reference (BKE_pchan_bbone_deform_clamp_segment_index):
 *   float pre_blend = head_tail * float(segments);
 *   int index = clamp(int(floor(pre_blend)), 0, segments - 1);
 *   float blend = clamp(pre_blend - index, 0, 1);
 *
 * CPU reference (accumulate_bbone):
 *   pose_mats[index + 1]  // +1 skips the padding
 *
 * bone_rest_length[bone_idx] = bone->length (rest length, constant, uploaded once)
 */
mat4 get_bone_matrix(int bone_idx, vec3 co_armature_space) {
  int segments = bone_segments[bone_idx];
  int base_idx = bone_offsets[bone_idx];

  /* Simple bone: no padding, just chan_mat */
  if (segments == 1) {
    return bone_pose_mat[base_idx];
  }

  /* B-Bone straight mapping.
   * bone_pose_mat[base_idx + 0] = mats[0] = the space/padding matrix.
   * CPU: y = mat[0][1]*co[0] + mat[1][1]*co[1] + mat[2][1]*co[2] + mat[3][1]
   * GLSL mat4 is column-major: mat[col][row]
   * → mat[0][1] = col0.row1, mat[1][1] = col1.row1, etc. */
  mat4 space_mat = bone_pose_mat[base_idx]; /* mats[0] */

  float y = space_mat[0][1] * co_armature_space.x
          + space_mat[1][1] * co_armature_space.y
          + space_mat[2][1] * co_armature_space.z
          + space_mat[3][1];

  /* CPU uses bone->length (rest length), NOT the posed length.
   * bone_rest_length[] is uploaded once from bone->length. */
  float bone_length = bone_rest_length[bone_idx];

  /* BKE_pchan_bbone_deform_clamp_segment_index */
  float head_tail = (bone_length > 1e-4) ? (y / bone_length) : 0.0;
  head_tail = clamp(head_tail, 0.0, 1.0);

  float pre_blend = head_tail * float(segments);
  int index = int(floor(pre_blend));
  index = clamp(index, 0, segments - 1);
  float blend = clamp(pre_blend - float(index), 0.0, 1.0);

  /* accumulate_bbone: pose_mats[index + 1] and [index + 2] (+1 skips padding) */
  mat4 mat_a = bone_pose_mat[base_idx + index + 1];
  mat4 mat_b = bone_pose_mat[base_idx + index + 2];

  return mat_a * (1.0 - blend) + mat_b * blend;
}

vec4 skin_pos_object(int v_idx) {
  /* Transform to armature space first, matching CPU:
   * co = math::transform_point(params.target_to_armature, co) */
  vec4 rest_pos_object = premat[0] * skinned_vert_positions_in[v_idx];

  int start_idx = in_offsets[v_idx];
  int end_idx   = in_offsets[v_idx + 1];
  int influence_count = end_idx - start_idx;

  /* No influences → rest pose (identity deformation) */
  if (influence_count == 0) {
    return rest_pos_object;
  }

  /* BoneDeformLinearMixer: accumulate deltas, then normalize by total.
   * CPU: position_delta += weight * (transform_point(pose_mat, co) - co)
   * CPU finalize: co += position_delta * (armature_weight / total) */
  vec4 position_delta = vec4(0.0);
  float total = 0.0;

  for (int i = 0; i < influence_count; ++i) {
    int idx = start_idx + i;
    int b   = in_idx[idx];
    float w = in_wgt[idx];

    if (w > 0.0) {
      mat4 bone_mat = get_bone_matrix(b, rest_pos_object.xyz);
      position_delta += w * (bone_mat * rest_pos_object - rest_pos_object);
      total += w;
    }
  }

  /* CPU: contrib_threshold = 0.0001 */
  if (total <= CONTRIB_THRESHOLD) {
    return rest_pos_object;
  }

  /* Normalize by total, matching finalize(armature_weight=1, total).
   * armature_weight (modifier mask) is applied in main() via mix(). */
  return rest_pos_object + position_delta / total;
}

void main() {
  uint v = gl_GlobalInvocationID.x;
  if (v >= skinned_vert_positions_out.length()) {
    return;
  }

  /* Modifier vertex group mask weight (armature_weight in CPU terms).
   * CPU: armature_weight = invert ? 1-mask : mask
   * Applied here as blend factor, equivalent to finalize(armature_weight/total). */
  float modifier_weight = 1.0;
  if (vgroup_weights.length() > 0 && v < vgroup_weights.length()) {
    modifier_weight = vgroup_weights[v];
  }

  if (modifier_weight < 1e-6) {
    /* armature_weight == 0: vertex stays at rest pose, transform through spaces */
    skinned_vert_positions_out[v] = postmat[0] * (premat[0] * skinned_vert_positions_in[v]);
    return;
  }

  vec4 skinned = skin_pos_object(int(v));
  vec4 rest    = premat[0] * skinned_vert_positions_in[v];

  /* mix(rest, skinned, modifier_weight) == rest + (skinned - rest) * modifier_weight
   * == co + delta/total * modifier_weight
   * CPU equivalent: co += position_delta * (armature_weight / total) */
  skinned_vert_positions_out[v] = postmat[0] * mix(rest, skinned, modifier_weight);
}
)GLSL";

/* Dual Quaternion Skinning shader with B-Bone support */
static const char *skin_compute_dqs_src = R"GLSL(
#ifndef CONTRIB_THRESHOLD
  #define CONTRIB_THRESHOLD 1e-4
#endif

/* ============================================================
 * QUATERNION HELPERS
 * CPU reference: dot_qtqt
 * Note: CPU quaternion layout is [w, x, y, z]
 * We store and work in the same [w, x, y, z] layout throughout,
 * only the final mul_v3m3_dq unpacks them.
 * ============================================================ */

float dot_qtqt(vec4 a, vec4 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

/* ============================================================
 * B-BONE SEGMENT INDEX (shared logic with LBS)
 *
 * CPU reference (find_bbone_segment_index_straight):
 *   const Mat4 *mats = pchan->runtime.bbone_deform_mats;
 *   const float (*mat)[4] = mats[0].mat;
 *   const float y = mat[0][1]*co[0] + mat[1][1]*co[1] + mat[2][1]*co[2] + mat[3][1];
 *   BKE_pchan_bbone_deform_clamp_segment_index(bone, y / bone->length, &index, &blend);
 *
 * IMPORTANT: bone_bbone_space_mat[] contains bbone_deform_mats[0] for each bone,
 *            uploaded separately (see C++ upload_bbone_space_matrices).
 *            bone_rest_length[] contains bone->length (rest length, constant).
 * ============================================================ */
void find_bbone_segment_index_straight(int bone_idx,
                                       vec3 co_armature_space,
                                       out int r_index,
                                       out float r_blend_next)
{
  /* mats[0] = the space matrix, stored in bone_bbone_space_mat[bone_idx] */
  mat4 space_mat = bone_bbone_space_mat[bone_idx];

  /* CPU: y = mat[0][1]*co[0] + mat[1][1]*co[1] + mat[2][1]*co[2] + mat[3][1]
   * GLSL mat4 column-major: mat[col][row] */
  float y = space_mat[0][1] * co_armature_space.x
          + space_mat[1][1] * co_armature_space.y
          + space_mat[2][1] * co_armature_space.z
          + space_mat[3][1];

  /* CPU: y / bone->length → head_tail in [0, 1] */
  float bone_length = bone_rest_length[bone_idx];
  float head_tail = (bone_length > 1e-4) ? (y / bone_length) : 0.0;
  head_tail = clamp(head_tail, 0.0, 1.0);

  /* BKE_pchan_bbone_deform_clamp_segment_index */
  int segments = bone_segments[bone_idx];
  float pre_blend = head_tail * float(segments);
  r_index = clamp(int(floor(pre_blend)), 0, segments - 1);
  r_blend_next = clamp(pre_blend - float(r_index), 0.0, 1.0);
}

/* ============================================================
 * add_weighted_dq_dq_pivot (GPU port)
 *
 * CPU reference:
 *   if (dq->scale_weight) {
 *     float dst[3];
 *     mul_v3_m4v3(dst, mdq.scale, pivot);  // dst = scale_mat * pivot
 *     sub_v3_v3(dst, pivot);               // dst = dst - pivot
 *     // Adjust trans to account for scale translation at pivot
 *     mdq.trans[0] -= .5f*(mdq.quat[1]*dst[0] + mdq.quat[2]*dst[1] + mdq.quat[3]*dst[2]);
 *     mdq.trans[1] += .5f*(mdq.quat[0]*dst[0] + mdq.quat[2]*dst[2] - mdq.quat[3]*dst[1]);
 *     mdq.trans[2] += .5f*(mdq.quat[0]*dst[1] + mdq.quat[3]*dst[0] - mdq.quat[1]*dst[2]);
 *     mdq.trans[3] += .5f*(mdq.quat[0]*dst[2] + mdq.quat[1]*dst[1] - mdq.quat[2]*dst[0]);
 *     sub_v3_v3(mdq.scale[3], dst);  // translate scale matrix to pivot
 *   }
 *   add_weighted_dq_dq(dq_sum, &mdq, weight);
 *
 * DualQuat layout [w, x, y, z] matching CPU.
 *
 * We pass scale as mat4 (4th column = translation row in Blender's column-major layout).
 * scale[3] = translation column of the 4x4 scale matrix.
 *
 * Note: compute_scale_matrix=true branch only (we always compute scale for correctness).
 * ============================================================ */
void add_weighted_dq_dq_pivot(
  inout vec4 dq_sum_quat,   /* [w,x,y,z] */
  inout vec4 dq_sum_trans,  /* [w,x,y,z] */
  inout mat4 dq_sum_scale,
  inout float dq_sum_scale_weight,
  vec4 bone_quat,           /* [w,x,y,z] */
  vec4 bone_trans,          /* [w,x,y,z] */
  mat4 bone_scale,
  float bone_scale_weight,
  vec3 pivot,
  float weight)
{
  /* CPU: make sure we interpolate quats in the right direction */
  if (dot_qtqt(bone_quat, dq_sum_quat) < 0.0) {
    weight = -weight;
  }

  if (bone_scale_weight > 0.0) {
    /* mdq = copy of bone dq, will be modified for pivot */
    vec4 mdq_quat  = bone_quat;
    vec4 mdq_trans = bone_trans;
    mat4 mdq_scale = bone_scale;

    /* CPU: dst = mul_v3_m4v3(scale_mat, pivot) - pivot
     * i.e. apply the 4x4 scale matrix to pivot (as a point), then subtract pivot.
     * In GLSL column-major mat4: mat * vec4(pivot, 1.0) */
    vec3 dst = (mdq_scale * vec4(pivot, 1.0)).xyz - pivot;

    /* CPU: adjust trans (quat layout: [w=0, x=1, y=2, z=3]) */
    mdq_trans.x -= 0.5 * (mdq_quat.y * dst.x + mdq_quat.z * dst.y + mdq_quat.w * dst.z);
    mdq_trans.y += 0.5 * (mdq_quat.x * dst.x + mdq_quat.z * dst.z - mdq_quat.w * dst.y);
    mdq_trans.z += 0.5 * (mdq_quat.x * dst.y + mdq_quat.w * dst.x - mdq_quat.y * dst.z);
    mdq_trans.w += 0.5 * (mdq_quat.x * dst.z + mdq_quat.y * dst.y - mdq_quat.z * dst.x);

    /* CPU: sub_v3_v3(mdq.scale[3], dst)
     * scale[3] is the translation column of the scale matrix */
    mdq_scale[3].xyz -= dst;

    /* add_weighted_dq_dq: accumulate quat and trans */
    dq_sum_quat  += weight * mdq_quat;
    dq_sum_trans += weight * mdq_trans;

    /* accumulate scale (use abs(weight) for scale, as in CPU for flipped case) */
    float scale_w = abs(weight);
    dq_sum_scale        += scale_w * mdq_scale;
    dq_sum_scale_weight += scale_w;
  }
  else {
    /* No scale: just accumulate quat and trans */
    dq_sum_quat  += weight * bone_quat;
    dq_sum_trans += weight * bone_trans;
    /* scale_weight stays untouched: normalize_dq will compensate */
  }
}

/* ============================================================
 * normalize_dq (GPU port)
 *
 * CPU reference:
 *   mul_qt_fl(dq->quat, 1/totweight);
 *   mul_qt_fl(dq->trans, 1/totweight);
 *   if (dq->scale_weight) {
 *     float addweight = totweight - dq->scale_weight;
 *     if (addweight) { dq->scale[i][i] += addweight; }
 *     mul_m4_fl(dq->scale, 1/totweight);
 *     dq->scale_weight = 1.0f;
 *   }
 * ============================================================ */
void normalize_dq(inout vec4 dq_quat,
                  inout vec4 dq_trans,
                  inout mat4 dq_scale,
                  inout float dq_scale_weight,
                  float totweight)
{
  float scale = 1.0 / totweight;
  dq_quat  *= scale;
  dq_trans *= scale;

  if (dq_scale_weight > 0.0) {
    float addweight = totweight - dq_scale_weight;
    if (addweight > 0.0) {
      dq_scale[0][0] += addweight;
      dq_scale[1][1] += addweight;
      dq_scale[2][2] += addweight;
      dq_scale[3][3] += addweight;
    }
    dq_scale        *= scale;
    dq_scale_weight  = 1.0;
  }
}

/* ============================================================
 * mul_v3m3_dq (GPU port)
 *
 * CPU reference layout: quat=[w,x,y,z], trans=[w,x,y,z]
 * We match this exactly.
 * ============================================================ */
vec3 mul_v3_dq(vec3 co,
               vec4 dq_quat,   /* [w,x,y,z] */
               vec4 dq_trans,  /* [w,x,y,z] */
               mat4 dq_scale,
               float dq_scale_weight)
{
  float w  = dq_quat.x,  x  = dq_quat.y,  y  = dq_quat.z,  z  = dq_quat.w;
  float t0 = dq_trans.x, t1 = dq_trans.y, t2 = dq_trans.z, t3 = dq_trans.w;

  /* CPU rotation matrix M (column-major in GLSL: M[col][row]) */
  mat3 M;
  M[0][0] = w*w + x*x - y*y - z*z;
  M[1][0] = 2.0*(x*y - w*z);
  M[2][0] = 2.0*(x*z + w*y);

  M[0][1] = 2.0*(x*y + w*z);
  M[1][1] = w*w + y*y - x*x - z*z;
  M[2][1] = 2.0*(y*z - w*x);

  M[0][2] = 2.0*(x*z - w*y);
  M[1][2] = 2.0*(y*z + w*x);
  M[2][2] = w*w + z*z - x*x - y*y;

  float len2 = dot_qtqt(dq_quat, dq_quat);
  if (len2 > 0.0) {
    len2 = 1.0 / len2;
  }

  /* CPU: t[] = translation extracted from dual part */
  vec3 t;
  t.x = 2.0*(-t0*x + w*t1 - t2*z + y*t3);
  t.y = 2.0*(-t0*y + t1*z - x*t3 + w*t2);
  t.z = 2.0*(-t0*z + x*t2 + w*t3 - t1*y);

  vec3 result = co;

  /* CPU: if (dq->scale_weight) mul_m4_v3(dq->scale, r) */
  if (dq_scale_weight > 0.0) {
    result = (dq_scale * vec4(result, 1.0)).xyz;
  }

  /* CPU: mul_m3_v3(M, r); r[i] = (r[i] + t[i]) * len2 */
  result = M * result;
  result = (result + t) * len2;

  return result;
}

/* ============================================================
 * get_bone_dual_quat: B-Bone aware DQ lookup
 *
 * Uses find_bbone_segment_index_straight (same as LBS) for segment index,
 * then blends between quats[index] and quats[index+1].
 *
 * CPU reference (accumulate_bbone):
 *   const Span<DualQuat> quats = {pchan->runtime.bbone_dual_quats, segments + 1};
 *   add_weighted_dq_dq_pivot(&dq, &quats[index],     co, weight*(1-blend), ...);
 *   add_weighted_dq_dq_pivot(&dq, &quats[index + 1], co, weight*blend,     ...);
 *
 * DQS B-Bone array: segments+1 entries, NO padding (unlike LBS segments+2).
 * bone_offsets[] = bone_segment_offsets_dqs[] for DQS.
 * ============================================================ */
void get_bone_dq_pair(int bone_idx,
                      vec3 co_armature_space,
                      out vec4 out_quat_a, out vec4 out_trans_a,
                      out mat4 out_scale_a, out float out_scale_weight_a,
                      out vec4 out_quat_b, out vec4 out_trans_b,
                      out mat4 out_scale_b, out float out_scale_weight_b,
                      out float out_blend)
{
  int segments = bone_segments[bone_idx];
  int base_idx = bone_offsets[bone_idx];

  if (segments == 1) {
    /* Simple bone: index=0, blend=0 → only mat_a matters */
    out_quat_a         = bone_dq_quat[base_idx];
    out_trans_a        = bone_dq_trans[base_idx];
    out_scale_a        = bone_dq_scale[base_idx];
    out_scale_weight_a = bone_dq_scale_weight[base_idx];
    out_quat_b         = out_quat_a;
    out_trans_b        = out_trans_a;
    out_scale_b        = out_scale_a;
    out_scale_weight_b = out_scale_weight_a;
    out_blend          = 0.0;
    return;
  }

  /* B-Bone: find segment using the same mats[0] logic as LBS */
  int index;
  float blend;
  find_bbone_segment_index_straight(bone_idx, co_armature_space, index, blend);

  /* DQS: quats[index] and quats[index+1], NO +1 padding offset */
  out_quat_a         = bone_dq_quat[base_idx + index];
  out_trans_a        = bone_dq_trans[base_idx + index];
  out_scale_a        = bone_dq_scale[base_idx + index];
  out_scale_weight_a = bone_dq_scale_weight[base_idx + index];

  out_quat_b         = bone_dq_quat[base_idx + index + 1];
  out_trans_b        = bone_dq_trans[base_idx + index + 1];
  out_scale_b        = bone_dq_scale[base_idx + index + 1];
  out_scale_weight_b = bone_dq_scale_weight[base_idx + index + 1];

  out_blend = blend;
}

/* ============================================================
 * skin_pos_object: main DQS deformation
 *
 * CPU reference (armature_vert_task_with_mixer, DQS path):
 *   for each influence:
 *     pchan_bone_deform → b_bone_deform or accumulate
 *       → add_weighted_dq_dq_pivot(&dq, &quats[index], co, w*(1-blend), pivot=co)
 *       → add_weighted_dq_dq_pivot(&dq, &quats[index+1], co, w*blend, pivot=co)
 *   normalize_dq(&dq, total)
 *   mul_v3m3_dq(co, ...)
 *   co += (result - co) * armature_weight
 * ============================================================ */
vec4 skin_pos_object(int v_idx) {
  vec3 co = (premat[0] * skinned_vert_positions_in[v_idx]).xyz;

  int start_idx       = in_offsets[v_idx];
  int end_idx         = in_offsets[v_idx + 1];
  int influence_count = end_idx - start_idx;

  if (influence_count == 0) {
    return vec4(co, 1.0);
  }

  /* Accumulated dual quaternion - CPU layout [w,x,y,z] */
  vec4  dq_sum_quat         = vec4(0.0);
  vec4  dq_sum_trans        = vec4(0.0);
  mat4  dq_sum_scale        = mat4(0.0);
  float dq_sum_scale_weight = 0.0;
  float total               = 0.0;

  for (int i = 0; i < influence_count; ++i) {
    int   idx = start_idx + i;
    int   b   = in_idx[idx];
    float w   = in_wgt[idx];

    if (w <= 0.0) {
      continue;
    }

    /* Get the two B-Bone segment DQs and blend factor */
    vec4  quat_a; vec4  trans_a; mat4  scale_a; float sw_a;
    vec4  quat_b; vec4  trans_b; mat4  scale_b; float sw_b;
    float blend;
    get_bone_dq_pair(b, co,
                     quat_a, trans_a, scale_a, sw_a,
                     quat_b, trans_b, scale_b, sw_b,
                     blend);

    /* CPU b_bone_deform:
     *   add_weighted_dq_dq_pivot(&dq, &quats[index],   co, w*(1-blend), pivot=co)
     *   add_weighted_dq_dq_pivot(&dq, &quats[index+1], co, w*blend,     pivot=co)
     * For simple bones (blend=0), only the first call matters. */
    float w_a = w * (1.0 - blend);
    float w_b = w * blend;

    add_weighted_dq_dq_pivot(dq_sum_quat, dq_sum_trans, dq_sum_scale, dq_sum_scale_weight,
                              quat_a, trans_a, scale_a, sw_a,
                              co, w_a);

    if (blend > 0.0) {
      add_weighted_dq_dq_pivot(dq_sum_quat, dq_sum_trans, dq_sum_scale, dq_sum_scale_weight,
                                quat_b, trans_b, scale_b, sw_b,
                                co, w_b);
    }

    total += w;
  }

  if (total <= CONTRIB_THRESHOLD) {
    return vec4(co, 1.0);
  }

  /* normalize_dq */
  normalize_dq(dq_sum_quat, dq_sum_trans, dq_sum_scale, dq_sum_scale_weight, total);

  /* mul_v3m3_dq: transform co */
  vec3 result = mul_v3_dq(co, dq_sum_quat, dq_sum_trans, dq_sum_scale, dq_sum_scale_weight);

  /* CPU finalize: r_delta_co = (result - co) * armature_weight
   * armature_weight applied in main() via mix() */
  return vec4(result, 1.0);
}

void main() {
  uint v = gl_GlobalInvocationID.x;
  if (v >= skinned_vert_positions_out.length()) {
    return;
  }

  float modifier_weight = 1.0;
  if (vgroup_weights.length() > 0 && v < vgroup_weights.length()) {
    modifier_weight = vgroup_weights[v];
  }

  if (modifier_weight < 1e-6) {
    skinned_vert_positions_out[v] = postmat[0] * (premat[0] * skinned_vert_positions_in[v]);
    return;
  }

  vec4 skinned = skin_pos_object(int(v));
  vec4 rest    = premat[0] * skinned_vert_positions_in[v];

  /* CPU: co += (result - co) * armature_weight → mix(rest, skinned, modifier_weight) */
  skinned_vert_positions_out[v] = postmat[0] * mix(rest, skinned, modifier_weight);
}
)GLSL";

/* ============================================================================
 * B-BONE HELPER
 * FUNCTIONS
 * ============================================================================ */

namespace {

/* Compute hash of B-Bone configuration to detect changes */
static uint32_t compute_bbone_config_hash(Object *arm_ob)
{
  if (!arm_ob || !arm_ob->pose) {
    return 0;
  }

  uint32_t hash = 0;
  for (bPoseChannel *pchan = (bPoseChannel *)arm_ob->pose->chanbase.first; pchan;
       pchan = pchan->next)
  {
    bArmature *armature = id_cast<bArmature *>(arm_ob->data);
    Bone *bone = pchan->bone_get(*armature);
    if (!(bone->flag & BONE_NO_DEFORM)) {
      hash = BLI_hash_int_2d(hash, bone->segments);
    }
  }
  return hash;
}

/* Compute B-Bone segment info (offsets for both LBS and DQS) */
static void compute_bbone_segment_info(Object *arm_ob,
                                       int bone_count,
                                       std::vector<int> &out_segment_counts,
                                       std::vector<int> &out_offsets_lbs,
                                       std::vector<int> &out_offsets_dqs,
                                       int &out_total_lbs,
                                       int &out_total_dqs)
{
  out_segment_counts.resize(bone_count);
  out_offsets_lbs.resize(bone_count);
  out_offsets_dqs.resize(bone_count);

  out_total_lbs = 0;
  out_total_dqs = 0;

  int bi = 0;
  for (bPoseChannel *pchan = (bPoseChannel *)arm_ob->pose->chanbase.first; pchan;
       pchan = pchan->next)
  {
    bArmature *armature = id_cast<bArmature *>(arm_ob->data);
    Bone *bone = pchan->bone_get(*armature);

    if (bone->flag & BONE_NO_DEFORM) {
      continue;
    }

    const int segments = bone->segments;
    out_segment_counts[bi] = segments;

    /* LBS offsets (segments+2 for B-Bones with padding) */
    out_offsets_lbs[bi] = out_total_lbs;
    out_total_lbs += (segments > 1) ? (segments + 2) : 1;

    /* DQS offsets (segments+1 for B-Bones without padding) */
    out_offsets_dqs[bi] = out_total_dqs;
    out_total_dqs += (segments > 1) ? (segments + 1) : 1;

    bi++;
  }
}

/* Upload bone rest lengths for LBS (bone->length, constant) */
static void upload_bone_rest_lengths(Object *arm_ob,
                                     int bone_count,
                                     Mesh *mesh_owner,
                                     Object *deformed_eval,
                                     const std::string &key_lengths)
{
  gpu::StorageBuf *ssbo = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_lengths);
  if (ssbo) {
    return; /* already uploaded, rest lengths never change */
  }

  ssbo = bke::BKE_mesh_gpu_internal_ssbo_ensure(
      mesh_owner, deformed_eval, key_lengths, sizeof(float) * bone_count);
  if (!ssbo) {
    return;
  }

  std::vector<float> lengths(bone_count);
  int bi = 0;
  for (bPoseChannel *pchan = (bPoseChannel *)arm_ob->pose->chanbase.first; pchan;
       pchan = pchan->next)
  {
    bArmature *armature = id_cast<bArmature *>(arm_ob->data);
    Bone *bone = pchan->bone_get(*armature);
    if (bone->flag & BONE_NO_DEFORM) {
      continue;
    }
    lengths[bi++] = bone->length; /* rest length, constant */
  }

  GPU_storagebuf_update(ssbo, lengths.data());
}

/* Upload bone matrices for LBS */
static void upload_bone_matrices_lbs(Object *arm_ob,
                                     int total_segments,
                                     const std::vector<int> &segment_counts,
                                     Mesh *mesh_owner,
                                     Object *deformed_eval,
                                     const std::string &key_matrices)
{
  gpu::StorageBuf *ssbo_mat = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_matrices);
  if (!ssbo_mat) {
    ssbo_mat = bke::BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, deformed_eval, key_matrices, sizeof(float) * 16 * total_segments);
  }

  if (ssbo_mat) {
    std::vector<float> mats(total_segments * 16);

    int bi = 0;
    int mat_idx = 0;
    for (bPoseChannel *pchan = (bPoseChannel *)arm_ob->pose->chanbase.first; pchan;
         pchan = pchan->next)
    {
      bArmature *armature = id_cast<bArmature *>(arm_ob->data);
      Bone *bone = pchan->bone_get(*armature);

      if (bone->flag & BONE_NO_DEFORM) {
        continue;
      }

      const int segments = segment_counts[bi];

      if (segments > 1 && pchan->runtime.bbone_segments == segments &&
          pchan->runtime.bbone_deform_mats)
      {
        /* B-Bone: Upload segments+2 matrices (with padding) */
        for (int seg = 0; seg < segments + 2; seg++) {
          memcpy(&mats[mat_idx * 16], &pchan->runtime.bbone_deform_mats[seg], sizeof(float) * 16);
          mat_idx++;
        }
      }
      else {
        /* Simple bone */
        memcpy(&mats[mat_idx * 16], pchan->chan_mat, sizeof(float) * 16);
        mat_idx++;
      }

      bi++;
    }

    GPU_storagebuf_update(ssbo_mat, mats.data());
  }
}

/* Upload mats[0] (bbone space matrix (pchanbone.pchan->runtime.bbone_deform_mats))
 * for each bone — DQS.
 * CPU reference: find_bbone_segment_index_straight uses bbone_deform_mats[0]
 * This is the rest-space transform used to project co onto the bone Y axis.
 * Uploaded once (rest data), but bbone_deform_mats is recomputed each frame
 * so we update every frame to stay in sync. */
static void upload_bbone_space_matrices_dqs(Object *arm_ob,
                                            int bone_count,
                                            const std::vector<int> &segment_counts,
                                            Mesh *mesh_owner,
                                            Object *deformed_eval,
                                            const std::string &key_space_mat)
{
  /* These are per-bone (one entry per bone), not per-segment */
  gpu::StorageBuf *ssbo_space = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_space_mat);
  if (!ssbo_space) {
    ssbo_space = bke::BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, deformed_eval, key_space_mat, sizeof(float) * 16 * bone_count);
  }

  if (!ssbo_space) {
    return;
  }

  std::vector<float> space_mats(bone_count * 16, 0.0f);

  int bi = 0;
  for (bPoseChannel *pchan = (bPoseChannel *)arm_ob->pose->chanbase.first; pchan;
       pchan = pchan->next)
  {
    bArmature *armature = id_cast<bArmature *>(arm_ob->data);
    Bone *bone = pchan->bone_get(*armature);

    if (bone->flag & BONE_NO_DEFORM) {
      continue;
    }

    const int segments = segment_counts[bi];
    if (segments > 1 && pchan->runtime.bbone_segments == segments &&
        pchan->runtime.bbone_deform_mats)
    {
      /* mats[0] is the space/padding matrix used by find_bbone_segment_index_straight */
      memcpy(&space_mats[bi * 16], &pchan->runtime.bbone_deform_mats[0], sizeof(float) * 16);
    }
    else {
      /* Simple bone: space_mat not used for segment lookup, but upload identity
       * to avoid reading garbage in the shader (segments == 1 path returns early). */
      float identity[4][4];
      unit_m4(identity);
      memcpy(&space_mats[bi * 16], identity, sizeof(float) * 16);
    }

    bi++;
  }

  GPU_storagebuf_update(ssbo_space, space_mats.data());
}

/* Upload dual quaternions for DQS */
static void upload_bone_dual_quats_dqs(Object *arm_ob,
                                       int total_segments,
                                       const std::vector<int> &segment_counts,
                                       Mesh *mesh_owner,
                                       Object *deformed_eval,
                                       const std::string &key_quat,
                                       const std::string &key_trans,
                                       const std::string &key_scale,
                                       const std::string &key_scale_weight)
{
  gpu::StorageBuf *ssbo_quat = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_quat);
  if (!ssbo_quat) {
    ssbo_quat = bke::BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, deformed_eval, key_quat, sizeof(float) * 4 * total_segments);
  }

  gpu::StorageBuf *ssbo_trans = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_trans);
  if (!ssbo_trans) {
    ssbo_trans = bke::BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, deformed_eval, key_trans, sizeof(float) * 4 * total_segments);
  }

  gpu::StorageBuf *ssbo_scale = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_scale);
  if (!ssbo_scale) {
    ssbo_scale = bke::BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, deformed_eval, key_scale, sizeof(float) * 16 * total_segments);
  }

  gpu::StorageBuf *ssbo_scale_weight = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner,
                                                                               key_scale_weight);
  if (!ssbo_scale_weight) {
    ssbo_scale_weight = bke::BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, deformed_eval, key_scale_weight, sizeof(float) * total_segments);
  }

  if (ssbo_quat && ssbo_trans && ssbo_scale && ssbo_scale_weight) {
    std::vector<float> quats(total_segments * 4);
    std::vector<float> trans(total_segments * 4);
    std::vector<float> scales(total_segments * 16);
    std::vector<float> scale_weights(total_segments);

    int bi = 0;
    int dq_idx = 0;
    for (bPoseChannel *pchan = (bPoseChannel *)arm_ob->pose->chanbase.first; pchan;
         pchan = pchan->next)
    {
      bArmature *armature = id_cast<bArmature *>(arm_ob->data);
      Bone *bone = pchan->bone_get(*armature);

      if (bone->flag & BONE_NO_DEFORM) {
        continue;
      }

      const int segments = segment_counts[bi];

      /* Compute deform_dual_quat for simple bones */
      float imat[4][4];
      if (bone) {
        invert_m4_m4(imat, bone->arm_mat);
        mul_m4_m4m4(pchan->chan_mat, pchan->pose_mat, imat);
        mat4_to_dquat(&pchan->runtime.deform_dual_quat, bone->arm_mat, pchan->chan_mat);
      }

      if (segments > 1 && pchan->runtime.bbone_segments == segments &&
          pchan->runtime.bbone_dual_quats)
      {
        /* B-Bone: Upload segments+1 DQs (no padding) */
        for (int seg = 0; seg < segments + 1; seg++) {
          const DualQuat &dq = pchan->runtime.bbone_dual_quats[seg];

          quats[dq_idx * 4 + 0] = dq.quat[0];
          quats[dq_idx * 4 + 1] = dq.quat[1];
          quats[dq_idx * 4 + 2] = dq.quat[2];
          quats[dq_idx * 4 + 3] = dq.quat[3];

          trans[dq_idx * 4 + 0] = dq.trans[0];
          trans[dq_idx * 4 + 1] = dq.trans[1];
          trans[dq_idx * 4 + 2] = dq.trans[2];
          trans[dq_idx * 4 + 3] = dq.trans[3];

          memcpy(&scales[dq_idx * 16], dq.scale, sizeof(float) * 16);
          scale_weights[dq_idx] = dq.scale_weight;

          dq_idx++;
        }
      }
      else {
        /* Simple bone */
        const DualQuat &dq = pchan->runtime.deform_dual_quat;

        quats[dq_idx * 4 + 0] = dq.quat[0];
        quats[dq_idx * 4 + 1] = dq.quat[1];
        quats[dq_idx * 4 + 2] = dq.quat[2];
        quats[dq_idx * 4 + 3] = dq.quat[3];

        trans[dq_idx * 4 + 0] = dq.trans[0];
        trans[dq_idx * 4 + 1] = dq.trans[1];
        trans[dq_idx * 4 + 2] = dq.trans[2];
        trans[dq_idx * 4 + 3] = dq.trans[3];

        memcpy(&scales[dq_idx * 16], dq.scale, sizeof(float) * 16);
        scale_weights[dq_idx] = dq.scale_weight;

        dq_idx++;
      }

      bi++;
    }

    GPU_storagebuf_update(ssbo_quat, quats.data());
    GPU_storagebuf_update(ssbo_trans, trans.data());
    GPU_storagebuf_update(ssbo_scale, scales.data());
    GPU_storagebuf_update(ssbo_scale_weight, scale_weights.data());
  }
}

}  // namespace

/* ============================================================================
 * ARMATURE
 * SKINNING MANAGER IMPLEMENTATION
 *
 * ============================================================================ */

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
  // const bool use_dqs = (amd->deformflag & ARM_DEF_QUATERNION) != 0;
  // hash = BLI_hash_int_2d(hash, use_dqs ? 1 : 0);

  /* Hash vertex group name (if specified) - like Lattice modifier */
  if (amd->defgrp_name[0] != '\0') {
    hash = BLI_hash_string(amd->defgrp_name);
  }

  blender::Span<MDeformVert> dverts = mesh_orig->deform_verts();
  hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(dverts.data())));

  return hash;
}

void ArmatureSkinningManager::ensure_static_resources(const ArmatureModifierData *amd,
                                                      Object *arm_ob,
                                                      Object *deformed_ob,
                                                      Mesh *orig_mesh,
                                                      uint32_t pipeline_hash)
{
  (void)deformed_ob;
  if (!orig_mesh || !amd || !arm_ob) {
    return;
  }

  /* Use composite key (mesh, modifier_uid) to support multiple armatures per mesh */
  Impl::MeshModifierKey key{orig_mesh, uint32_t(amd->modifier.persistent_uid)};
  Impl::MeshStaticData &msd = impl_->static_map.lookup_or_add_default(key);

  /* Check if recalculation is needed by comparing pipeline hash.
   * The hash is computed by GPUModifierPipeline and includes ALL Armature state
   * (vertex count, armature pointer, DQS
   * mode, vertex groups, bone count).
   *
   * We recalculate CPU influences when:
   * 1. First
   * time (last_verified_hash == 0)
   * 2. Hash changed (pipeline_hash != last_verified_hash)
   */
  const bool first_time = (msd.last_verified_hash == 0);
  const bool hash_changed = (pipeline_hash != msd.last_verified_hash);

  if (!first_time && !hash_changed) {
    return;  // No changes detected, reuse cached influences
  }

  /* Recalculate influences (triggered by hash change or GPU invalidation) */
  if (0) {
    printf(
        "Recalculating Armature influences for mesh '%s' (first=%d, hash_changed=%d)\n",
        (orig_mesh->id.name + 2),
        first_time,
        hash_changed);
  }

  /* Update hash cache */
  msd.last_verified_hash = pipeline_hash;

  const int verts_num = orig_mesh->verts_num;
  msd.verts_num = verts_num;
  msd.in_influence_offsets.clear();
  msd.in_indices.clear();
  msd.in_weights.clear();

  msd.in_influence_offsets.resize(verts_num + 1, 0); /* +1 for end offset */

  /* Build group name -> bone index map from armature pose. */
  Map<std::string, int> bone_name_to_index;
  if (arm_ob && arm_ob->pose) {
    int idx = 0;
    for (bPoseChannel *pchan = (bPoseChannel *)arm_ob->pose->chanbase.first; pchan;
         pchan = pchan->next)
    {
      bArmature *armature = id_cast<bArmature *>(arm_ob->data);
      Bone *bone = pchan->bone_get(*armature);

      if (!(bone->flag & BONE_NO_DEFORM)) {
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

  /* Check if dverts is empty before accessing it!
   * When ALL vertex groups are deleted, dverts.data() == nullptr
   * Accessing dverts[v] would crash with Access Violation.
   *
   * If empty, all vertices get zero influences (rest pose). */
  if (dverts.is_empty()) {
    /* No deform verts: all vertices get zero influences (rest pose) */
    for (int v = 0; v <= verts_num; ++v) {
      msd.in_influence_offsets[v] = 0;  // All offsets point to start (no influences)
    }
    /* in_indices and in_weights remain empty (no influences) */
  }
  else {
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

      ///* Normalize weights */
      //if (total_weight > kContribThreshold) {
      //  const float inv_total = 1.0f / total_weight;
      //  for (auto &inf : influences) {
      //    inf.weight *= inv_total;
      //  }
      //}

      /* Store all influences (no 16-bone limit!) */
      for (const auto &inf : influences) {
        msd.in_indices[influence_idx] = inf.bone_idx;
        msd.in_weights[influence_idx] = inf.weight;
        influence_idx++;
      }
    }
  }

  /* Remember armature/deformed pointers so dispatch can compute premat/postmat. */
  msd.arm = arm_ob;
  msd.deformed = deformed_ob;

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

gpu::StorageBuf *ArmatureSkinningManager::dispatch_skinning(
    const ArmatureModifierData *amd,
    Depsgraph * /*depsgraph*/,
    Object *eval_armature_ob,
    Object *deformed_eval,
    MeshBatchCache *cache,
    gpu::StorageBuf *ssbo_in)
{
  if (!amd || !eval_armature_ob || !ssbo_in) {
    return nullptr;
  }

  Mesh *mesh_owner = (cache && cache->mesh_owner) ? cache->mesh_owner : nullptr;
  if (!mesh_owner) {
    return nullptr;
  }

  /* Use composite key (mesh, modifier_uid) to support multiple armatures per mesh
   * Pattern now matches Hook/Lattice/SimpleDeform/Displace (unified design) */
  Impl::MeshModifierKey key{mesh_owner, uint32_t(amd->modifier.persistent_uid)};
  Impl::MeshStaticData *msd_ptr = impl_->static_map.lookup_ptr(key);
  if (!msd_ptr) {
    return nullptr;
  }
  Impl::MeshStaticData &msd = *msd_ptr;

  /* Check if dual quaternion skinning is enabled (now using amd directly!) */
  const bool use_dual_quaternions = (amd->deformflag & ARM_DEF_QUATERNION) != 0;

  /* Create unique keys per (mesh, armature) using composite key hash.
   * This prevents conflicts when multiple meshes use the same armature.
   * Pattern matches Hook modifier: "armature_<hash>_<resource>" */
  const std::string key_prefix = "armature_" + std::to_string(key.hash()) + "_";

  const std::string key_in_idx = key_prefix + "in_idx";
  const std::string key_in_wgt = key_prefix + "in_wgt";
  const std::string key_in_offsets = key_prefix + "in_offsets";
  const std::string key_premat = key_prefix + "premat";
  const std::string key_postmat = key_prefix + "postmat";
  const std::string key_vgroup = key_prefix + "vgroup_weights";
  const std::string key_bone_rest_length = key_prefix + "bone_rest_length";

  /* Compute premat and postmat for coordinate space conversion */
  float premat[4][4], postmat[4][4], obinv[4][4];
  invert_m4_m4(obinv, deformed_eval->object_to_world().ptr());
  copy_m4_m4(premat, deformed_eval->object_to_world().ptr());
  mul_m4_m4m4(postmat, obinv, eval_armature_ob->object_to_world().ptr());
  invert_m4_m4(premat, postmat);

  /* ensure/upload per-mesh SSBOs (use GPU_storagebuf_update directly) */
  gpu::StorageBuf *ssbo_in_offsets = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner,
                                                                             key_in_offsets);
  if (!ssbo_in_offsets) {
    ssbo_in_offsets = bke::BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, deformed_eval, key_in_offsets, sizeof(int) * (msd.verts_num + 1));
    if (!ssbo_in_offsets) {
      return nullptr;
    }
    GPU_storagebuf_update(ssbo_in_offsets, msd.in_influence_offsets.data());
  }

  gpu::StorageBuf *ssbo_in_idx = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_in_idx);
  if (!ssbo_in_idx) {
    ssbo_in_idx = bke::BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, deformed_eval, key_in_idx, sizeof(int) * msd.in_indices.size());
    if (!ssbo_in_idx) {
      return nullptr;
    }
    GPU_storagebuf_update(ssbo_in_idx, msd.in_indices.data());
  }

  gpu::StorageBuf *ssbo_in_wgt = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_in_wgt);
  if (!ssbo_in_wgt) {
    ssbo_in_wgt = bke::BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, deformed_eval, key_in_wgt, sizeof(float) * msd.in_weights.size());
    if (!ssbo_in_wgt) {
      return nullptr;
    }
    GPU_storagebuf_update(ssbo_in_wgt, msd.in_weights.data());
  }

  /* Upload vertex group weights SSBO (modifier filter - like Lattice) */
  gpu::StorageBuf *ssbo_vgroup = modifier_gpu_helpers::ensure_vgroup_ssbo(
      mesh_owner, deformed_eval, key_vgroup, msd.vgroup_weights, msd.verts_num);

  /* Ensure per-modifier in/out skinned position SSBOs for chaining GPU modifiers. */
  const std::string key_skinned_out = key_prefix + "skinned_pos_out";

  gpu::StorageBuf *ssbo_skinned_out = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_skinned_out);
  if (!ssbo_skinned_out) {
    const size_t size_skinned = sizeof(float) * 4 * std::max(msd.verts_num, 1);
    ssbo_skinned_out = bke::BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, deformed_eval, key_skinned_out, size_skinned);
    /* leave uninitialized; shader will write full buffer */
  }

  gpu::StorageBuf *ssbo_premat = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_premat);
  if (!ssbo_premat) {
    ssbo_premat = bke::BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, deformed_eval, key_premat, sizeof(float) * 16);
    if (!ssbo_premat) {
      return nullptr;
    }
  }
  GPU_storagebuf_update(ssbo_premat, &premat[0][0]);

  gpu::StorageBuf *ssbo_postmat = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_postmat);
  if (!ssbo_postmat) {
    ssbo_postmat = bke::BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, deformed_eval, key_postmat, sizeof(float) * 16);
    if (!ssbo_postmat) {
      return nullptr;
    }
  }
  GPU_storagebuf_update(ssbo_postmat, &postmat[0][0]);

  /* dual quaternions + rest_length (lbs+dqs) */
  gpu::StorageBuf *ssbo_bone_rest_length = nullptr;

  gpu::StorageBuf *ssbo_bone_dq_quat = nullptr;
  gpu::StorageBuf *ssbo_bone_dq_trans = nullptr;
  gpu::StorageBuf *ssbo_bone_dq_scale = nullptr;
  gpu::StorageBuf *ssbo_bone_dq_scale_weight = nullptr;

  if (msd.arm) {
    /* Count deformable bones if not already cached */
    if (msd.bones == 0) {
      int bc = 0;
      for (bPoseChannel *pchan = (bPoseChannel *)msd.arm->pose->chanbase.first; pchan;
           pchan = pchan->next)
      {
        bArmature *armature = id_cast<bArmature *>(eval_armature_ob->data);
        Bone *bone = pchan->bone_get(*armature);

        if (!(bone->flag & BONE_NO_DEFORM)) {
          bc++;
        }
      }
      msd.bones = bc;
    }

    if (msd.bones > 0) {
      /* ====================================================================
       * B-BONE CONFIGURATION CHANGE DETECTION
       * ==================================================================== */

      /* Detect if B-Bone segment counts changed (e.g., user changed segments in UI).
       * This is a structural change that requires full resource recreation. */
      const uint32_t current_bbone_hash = compute_bbone_config_hash(msd.arm);
      if (msd.bbone_config_hash != 0 && current_bbone_hash != msd.bbone_config_hash) {
        /* B-Bone configuration changed! Invalidate all GPU resources and CPU data. */
        if (0) { /* Debug log (enable if needed) */
          printf("B-Bone config changed for mesh '%s'! Invalidating all resources.\n",
                 (mesh_owner->id.name + 2));
        }

        /* Invalidate everything: GPU buffers + CPU segment data */
        invalidate_all(mesh_owner);

        /* Clear segment data to force recalculation */
        msd.bone_segment_counts.clear();
        msd.bone_segment_offsets_lbs.clear();
        msd.bone_segment_offsets_dqs.clear();
        msd.total_segments_lbs = 0;
        msd.total_segments_dqs = 0;
        msd.bbone_config_hash = 0; /* Will be recalculated below */

        /* Return early - resources will be recreated on next frame */
        return nullptr;
      }

      /* ====================================================================
       * BONE DATA UPLOAD (LBS or DQS)
       * ==================================================================== */
      /* Keys for bone data are already generated above (key_prefix) */

      if (use_dual_quaternions) {
        /* ====================================================================
         * DQS MODE (Dual Quaternion Skinning with B-Bone support)
         * ==================================================================== */

        /* Define SSBO keys (shared with LBS for bone_segments) */
        const std::string key_bone_segments = key_prefix + "bone_segments";
        const std::string key_bone_offsets_dqs = key_prefix + "bone_offsets_dqs";
        const std::string key_dq_quat = key_prefix + "dq_quat";
        const std::string key_dq_trans = key_prefix + "dq_trans";
        const std::string key_dq_scale = key_prefix + "dq_scale";
        const std::string key_dq_scale_weight = key_prefix + "dq_scale_weight";
        const std::string key_bbone_space_mat = key_prefix + "dq_bbone_space_mat";

        /* Compute B-Bone segment info (if not already done) */
        if (msd.bone_segment_counts.empty()) {
          compute_bbone_segment_info(msd.arm,
                                     msd.bones,
                                     msd.bone_segment_counts,
                                     msd.bone_segment_offsets_lbs,
                                     msd.bone_segment_offsets_dqs,
                                     msd.total_segments_lbs,
                                     msd.total_segments_dqs);

          /* Cache B-Bone config hash to detect changes */
          msd.bbone_config_hash = compute_bbone_config_hash(msd.arm);
        }

        /* Upload bone_segments SSBO (shared, upload once) */
        gpu::StorageBuf *ssbo_bone_segments = bke::BKE_mesh_gpu_internal_ssbo_get(
            mesh_owner, key_bone_segments);
        if (!ssbo_bone_segments) {
          ssbo_bone_segments = bke::BKE_mesh_gpu_internal_ssbo_ensure(
              mesh_owner, deformed_eval, key_bone_segments, sizeof(int) * msd.bones);
          if (ssbo_bone_segments) {
            GPU_storagebuf_update(ssbo_bone_segments, msd.bone_segment_counts.data());
          }
        }

        /* Upload bone_offsets_dqs SSBO (DQS-specific, always update) */
        gpu::StorageBuf *ssbo_bone_offsets = bke::BKE_mesh_gpu_internal_ssbo_get(
            mesh_owner, key_bone_offsets_dqs);
        if (!ssbo_bone_offsets) {
          ssbo_bone_offsets = bke::BKE_mesh_gpu_internal_ssbo_ensure(
              mesh_owner, deformed_eval, key_bone_offsets_dqs, sizeof(int) * msd.bones);
        }
        if (ssbo_bone_offsets) {
          GPU_storagebuf_update(ssbo_bone_offsets, msd.bone_segment_offsets_dqs.data());
        }

        /* Upload every frame*/
        upload_bbone_space_matrices_dqs(eval_armature_ob, /* toujours eval, jamais msd.arm */
                                        msd.bones,
                                        msd.bone_segment_counts,
                                        mesh_owner,
                                        deformed_eval,
                                        key_bbone_space_mat);

        /* Upload dual quaternions (DQS-specific, update every frame) */
        upload_bone_dual_quats_dqs(msd.arm,
                                   msd.total_segments_dqs,
                                   msd.bone_segment_counts,
                                   mesh_owner,
                                   deformed_eval,
                                   key_dq_quat,
                                   key_dq_trans,
                                   key_dq_scale,
                                   key_dq_scale_weight);
        /* Upload 1 time*/
        upload_bone_rest_lengths(
            eval_armature_ob, msd.bones, mesh_owner, deformed_eval, key_bone_rest_length);

        /* Get SSBO pointers for binding */
        ssbo_bone_dq_quat = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_dq_quat);
        ssbo_bone_dq_trans = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_dq_trans);
        ssbo_bone_dq_scale = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_dq_scale);
        ssbo_bone_dq_scale_weight = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner,
                                                                   key_dq_scale_weight);

        /* Get SSBO pointer for binding */
        ssbo_bone_rest_length = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner,
                                                                    key_bone_rest_length);
      }
      else {
        /* ====================================================================
         * LBS MODE (Linear Blend Skinning with B-Bone support)
         * ==================================================================== */

        /* Define SSBO keys (shared bone_segments, LBS-specific offsets) */
        const std::string key_bone_segments = key_prefix + "bone_segments";
        const std::string key_bone_offsets_lbs = key_prefix + "bone_offsets_lbs";
        const std::string key_bone_pose = key_prefix + "bone_pose_lbs";

        /* Compute B-Bone segment info (if not already done) */
        if (msd.bone_segment_counts.empty()) {
          compute_bbone_segment_info(msd.arm,
                                     msd.bones,
                                     msd.bone_segment_counts,
                                     msd.bone_segment_offsets_lbs,
                                     msd.bone_segment_offsets_dqs,
                                     msd.total_segments_lbs,
                                     msd.total_segments_dqs);

          /* Cache B-Bone config hash to detect changes */
          msd.bbone_config_hash = compute_bbone_config_hash(msd.arm);
        }

        /* Upload bone_segments SSBO (shared, upload once) */
        gpu::StorageBuf *ssbo_bone_segments = bke::BKE_mesh_gpu_internal_ssbo_get(
            mesh_owner, key_bone_segments);
        if (!ssbo_bone_segments) {
          ssbo_bone_segments = bke::BKE_mesh_gpu_internal_ssbo_ensure(
              mesh_owner, deformed_eval, key_bone_segments, sizeof(int) * msd.bones);
          if (ssbo_bone_segments) {
            GPU_storagebuf_update(ssbo_bone_segments, msd.bone_segment_counts.data());
          }
        }

        /* Upload bone_offsets_lbs SSBO (LBS-specific, always update) */
        gpu::StorageBuf *ssbo_bone_offsets = bke::BKE_mesh_gpu_internal_ssbo_get(
            mesh_owner, key_bone_offsets_lbs);
        if (!ssbo_bone_offsets) {
          ssbo_bone_offsets = bke::BKE_mesh_gpu_internal_ssbo_ensure(
              mesh_owner, deformed_eval, key_bone_offsets_lbs, sizeof(int) * msd.bones);
        }
        if (ssbo_bone_offsets) {
          GPU_storagebuf_update(ssbo_bone_offsets, msd.bone_segment_offsets_lbs.data());
        }

        /* Upload bone matrices (LBS-specific, update every frame) */
        upload_bone_matrices_lbs(msd.arm,
                                 msd.total_segments_lbs,
                                 msd.bone_segment_counts,
                                 mesh_owner,
                                 deformed_eval,
                                 key_bone_pose);

        /* Upload 1 time*/
        upload_bone_rest_lengths(
            eval_armature_ob, msd.bones, mesh_owner, deformed_eval, key_bone_rest_length);

        /* Get SSBO pointer for binding */
        ssbo_bone_rest_length = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner,
                                                                    key_bone_rest_length);
      }
    }
  }
  const std::string shader_key = use_dual_quaternions ? "armature_skinning_dqs" :
                                                        "armature_skinning_lbs";
  gpu::Shader *compute_sh = bke::BKE_mesh_gpu_internal_shader_get(mesh_owner, shader_key);
  if (!compute_sh) {
    /* create/ensure compute shader and dispatch */
    using namespace gpu::shader;
    ShaderCreateInfo info("pyGPU_Shader");
    info.local_group_size(256, 1, 1);

    /* Select shader source based on skinning mode */
    if (use_dual_quaternions) {
      info.compute_source_generated = skin_compute_dqs_src;
      info.storage_buf(0, Qualifier::read, "vec4", "skinned_vert_positions_in[]");
      info.storage_buf(15, Qualifier::write, "vec4", "skinned_vert_positions_out[]");
      info.storage_buf(1, Qualifier::read, "int", "in_offsets[]");
      info.storage_buf(2, Qualifier::read, "int", "in_idx[]");
      info.storage_buf(3, Qualifier::read, "float", "in_wgt[]");
      info.storage_buf(4, Qualifier::read, "vec4", "bone_dq_quat[]");
      info.storage_buf(5, Qualifier::read, "vec4", "bone_dq_trans[]");
      info.storage_buf(6, Qualifier::read, "mat4", "bone_dq_scale[]");
      info.storage_buf(7, Qualifier::read, "float", "bone_dq_scale_weight[]");
      info.storage_buf(8, Qualifier::read, "mat4", "premat[]");
      info.storage_buf(9, Qualifier::read, "mat4", "postmat[]");
      info.storage_buf(10, Qualifier::read, "float", "vgroup_weights[]");  // Modifier filter
      info.storage_buf(11, Qualifier::read, "int", "bone_segments[]");  // B-Bone segments per bone
      info.storage_buf(12, Qualifier::read, "int", "bone_offsets[]");   // B-Bone DQ offsets
      info.storage_buf(13, Qualifier::read, "mat4", "bone_bbone_space_mat[]");
      info.storage_buf(14, Qualifier::read, "float", "bone_rest_length[]");
    }
    else {
      info.compute_source_generated = skin_compute_lbs_src;
      info.storage_buf(0, Qualifier::read, "vec4", "skinned_vert_positions_in[]");
      info.storage_buf(15, Qualifier::write, "vec4", "skinned_vert_positions_out[]");
      info.storage_buf(1, Qualifier::read, "int", "in_offsets[]");
      info.storage_buf(2, Qualifier::read, "int", "in_idx[]");
      info.storage_buf(3, Qualifier::read, "float", "in_wgt[]");
      info.storage_buf(4, Qualifier::read, "mat4", "bone_pose_mat[]");
      info.storage_buf(5, Qualifier::read, "mat4", "premat[]");
      info.storage_buf(6, Qualifier::read, "mat4", "postmat[]");
      info.storage_buf(7, Qualifier::read, "float", "vgroup_weights[]");  // Modifier filter
      info.storage_buf(8, Qualifier::read, "int", "bone_segments[]");  // B-Bone segments per bone
      info.storage_buf(9, Qualifier::read, "int", "bone_offsets[]");  // B-Bone matrix offsets
      info.storage_buf(10, Qualifier::read, "float", "bone_rest_length[]");
    }
    compute_sh = bke::BKE_mesh_gpu_internal_shader_ensure(mesh_owner, deformed_eval, shader_key, info);
  }

  if (!compute_sh) {
    return nullptr;
  }
  GPU_shader_bind(compute_sh);

  if (use_dual_quaternions) {
    /* Bind DQS buffers */
    GPU_storagebuf_bind(ssbo_in, 0);
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
    GPU_storagebuf_bind(ssbo_postmat, 9);
    if (ssbo_vgroup) {
      GPU_storagebuf_bind(ssbo_vgroup, 10);
    }
    /* Bind chained in/out buffers */
    gpu::StorageBuf *ssbo_skinned_out = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_prefix + "skinned_pos_out");
    if (ssbo_skinned_out) {
      GPU_storagebuf_bind(ssbo_skinned_out, 15); /* output binding */
    }

    /* Bind B-Bone segment info for DQS (using new separate keys) */
    const std::string key_bone_segments = key_prefix + "bone_segments";
    const std::string key_bone_offsets_dqs = key_prefix + "bone_offsets_dqs";
    const std::string key_bbone_space_mat = key_prefix + "dq_bbone_space_mat";

    gpu::StorageBuf *ssbo_bone_segments = bke::BKE_mesh_gpu_internal_ssbo_get(
        mesh_owner, key_bone_segments);
    gpu::StorageBuf *ssbo_bone_offsets_dqs = bke::BKE_mesh_gpu_internal_ssbo_get(
        mesh_owner, key_bone_offsets_dqs);
    gpu::StorageBuf *ssbo_bbone_space_mat = bke::BKE_mesh_gpu_internal_ssbo_get(
        mesh_owner, key_bbone_space_mat);

    if (ssbo_bone_segments) {
      GPU_storagebuf_bind(ssbo_bone_segments, 11);
    }
    if (ssbo_bone_offsets_dqs) {
      GPU_storagebuf_bind(ssbo_bone_offsets_dqs, 12);
    }
    if (ssbo_bbone_space_mat) {
      GPU_storagebuf_bind(ssbo_bbone_space_mat, 13);
    }
    if (ssbo_bone_rest_length) {
      GPU_storagebuf_bind(ssbo_bone_rest_length, 14);
    }
  }
  else {
    /* Bind LBS buffers (including B-Bone support) */

    const std::string key_bone_pose = key_prefix + "bone_pose_lbs";
    gpu::StorageBuf *ssbo_bone_mat = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner,
                                                                         key_bone_pose);
    GPU_storagebuf_bind(ssbo_in, 0);
    GPU_storagebuf_bind(ssbo_in_offsets, 1);
    GPU_storagebuf_bind(ssbo_in_idx, 2);
    GPU_storagebuf_bind(ssbo_in_wgt, 3);
    if (ssbo_bone_mat) {
      GPU_storagebuf_bind(ssbo_bone_mat, 4);
    }
    GPU_storagebuf_bind(ssbo_premat, 5);
    GPU_storagebuf_bind(ssbo_postmat, 6);
    if (ssbo_vgroup) {
      GPU_storagebuf_bind(ssbo_vgroup, 7);
    }
    /* Bind chained in/out buffers for LBS */
    gpu::StorageBuf *ssbo_skinned_out = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_prefix + "skinned_pos_out");
    if (ssbo_skinned_out) {
      GPU_storagebuf_bind(ssbo_skinned_out, 15);
    }

    /* Bind B-Bone segment info (using new separate keys) */
    const std::string key_bone_segments = key_prefix + "bone_segments";
    const std::string key_bone_offsets_lbs = key_prefix + "bone_offsets_lbs";

    gpu::StorageBuf *ssbo_bone_segments = bke::BKE_mesh_gpu_internal_ssbo_get(
        mesh_owner, key_bone_segments);
    gpu::StorageBuf *ssbo_bone_offsets_lbs = bke::BKE_mesh_gpu_internal_ssbo_get(
        mesh_owner, key_bone_offsets_lbs);

    if (ssbo_bone_segments) {
      GPU_storagebuf_bind(ssbo_bone_segments, 8);
    }
    if (ssbo_bone_offsets_lbs) {
      GPU_storagebuf_bind(ssbo_bone_offsets_lbs, 9);
    }
    if (ssbo_bone_rest_length) {
      GPU_storagebuf_bind(ssbo_bone_rest_length, 10);
    }
  }

  const int group_size = 256;
  int num_groups = (msd.verts_num + group_size - 1) / group_size;
  GPU_compute_dispatch(compute_sh, num_groups, 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  /* Return the SSBO containing the skinned positions. Caller will perform scatter if needed. */
  return ssbo_skinned_out;
}

void ArmatureSkinningManager::free_resources_for_mesh(Mesh *mesh)
{
  if (!mesh) {
    return;
  }

  /* Remove all entries for this mesh (may be multiple armatures) */
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

void ArmatureSkinningManager::invalidate_all(Mesh *mesh)
{
  if (!mesh) {
    return;
  }

  /* 1. Free all GPU resources (SSBOs + shaders) for this mesh */
  bke::BKE_mesh_gpu_internal_resources_free_for_mesh(mesh);
  /* Keep CPU data (influences etc.) for fast recreation */
}

void ArmatureSkinningManager::free_all()
{
  /* Clear CPU-side map. Per-mesh GPU resources are freed by BKE_mesh_gpu_free_all_caches()
   * or per-mesh frees elsewhere. */
  impl_->static_map.clear();
}

}  // namespace draw
}  // namespace blender
