/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_cast.hh"

#include "BLI_hash.h"
#include "BLI_map.hh"
#include "BLI_math_matrix.h"

#include "BKE_deform.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_gpu.hh"

#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

#include "GPU_compute.hh"
#include "GPU_shader.hh"

#include "../gpu/intern/gpu_shader_create_info.hh"
#include "draw_cache_extract.hh"

#include "draw_modifier_gpu_helpers.hh"

#include "GPU_storage_buffer.hh"

namespace blender {
namespace draw {

struct CastManager::Impl {
  /* Composite key: (Mesh*, modifier UID) to support multiple Cast modifiers per mesh */
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
    std::vector<float> vgroup_weights;    /* per-vertex weight (0.0-1.0) */
    int verts_num = 0;

    Object *ctrl_ob = nullptr; /* control object used by Cast */
    Object *deformed = nullptr;
    uint32_t last_verified_hash = 0;
  };

  Map<MeshModifierKey, MeshStaticData> static_map;
};

/* Reduction pass: compute per-workgroup sum of distances to center and counts.
 * Writes per-group results to group_sums[] and group_counts[] (float arrays).
 */

static const char *cast_reduction_src = R"GLSL(

#extension GL_EXT_shader_atomic_float : require

/* Helpers to pack floats for atomic min/max */
uint float_to_ordered_uint(float f) {
  uint u = floatBitsToUint(f);
  return (u & 0x80000000u) != 0u ? ~u : (u ^ 0x80000000u);
}

shared float local_sums[256];
shared uint local_counts[256];
shared float local_min_x[256];
shared float local_max_x[256];
shared float local_min_y[256];
shared float local_max_y[256];
shared float local_min_z[256];
shared float local_max_z[256];

void main() {
  const uint gid = gl_GlobalInvocationID.x;
  const uint lid = gl_LocalInvocationID.x;
  const uint group_size = gl_WorkGroupSize.x;
  const uint num_verts = input_positions.length();
  const uint stride = group_size * gl_NumWorkGroups.x;

  /* Compute center in object local space */
  vec3 center = vec3(0.0);
  if (has_ctrl) {
    /* ctrl_object_world and object_world are push constants (mat4) */
    mat4 obj_world = object_world;
    mat4 ctrl_world = ctrl_object_world;
    mat4 obj_world_inv = inverse(obj_world);
    center = (obj_world_inv * vec4(ctrl_world[3].xyz, 1.0)).xyz;
  }

  float tsum = 0.0;
  uint tcount = 0u;
  float tmin_x = 1.0/0.0; /* +INF */
  float tmax_x = -1.0/0.0; /* -INF */
  float tmin_y = 1.0/0.0;
  float tmax_y = -1.0/0.0;
  float tmin_z = 1.0/0.0;
  float tmax_z = -1.0/0.0;
  /* If control object exists, include its center in initial bounds (matches CPU ordering). */
  if (has_ctrl) {
    tmin_x = min(tmin_x, center.x);
    tmax_x = max(tmax_x, center.x);
    tmin_y = min(tmin_y, center.y);
    tmax_y = max(tmax_y, center.y);
    tmin_z = min(tmin_z, center.z);
    tmax_z = max(tmax_z, center.z);
  }
  for (uint i = gid; i < num_verts; i += stride) {
    vec3 pos = input_positions[i].xyz;
    vec3 rel = pos - center;
    float d = length(rel);
    if (d == d) { /* finite check */
      tsum += d;
      tcount += 1u;
      float vx = rel.x;
      float vy = rel.y;
      float vz = rel.z;
      if (vx == vx && abs(vx) < 1e30) {
        tmin_x = min(tmin_x, vx);
        tmax_x = max(tmax_x, vx);
      }
      if (vy == vy && abs(vy) < 1e30) {
        tmin_y = min(tmin_y, vy);
        tmax_y = max(tmax_y, vy);
      }
      if (vz == vz && abs(vz) < 1e30) {
        tmin_z = min(tmin_z, vz);
        tmax_z = max(tmax_z, vz);
      }
    }
  }

  local_sums[lid] = tsum;
  local_counts[lid] = tcount;
  local_min_x[lid] = tmin_x;
  local_max_x[lid] = tmax_x;
  local_min_y[lid] = tmin_y;
  local_max_y[lid] = tmax_y;
  local_min_z[lid] = tmin_z;
  local_max_z[lid] = tmax_z;

  barrier();
  memoryBarrierShared();

  for (uint s = group_size >> 1; s > 0; s >>= 1) {
    if (lid < s) {
      local_sums[lid] += local_sums[lid + s];
      local_counts[lid] += local_counts[lid + s];
      local_min_x[lid] = min(local_min_x[lid], local_min_x[lid + s]);
      local_max_x[lid] = max(local_max_x[lid], local_max_x[lid + s]);
      local_min_y[lid] = min(local_min_y[lid], local_min_y[lid + s]);
      local_max_y[lid] = max(local_max_y[lid], local_max_y[lid + s]);
      local_min_z[lid] = min(local_min_z[lid], local_min_z[lid + s]);
      local_max_z[lid] = max(local_max_z[lid], local_max_z[lid + s]);
    }
    barrier();
    memoryBarrierShared();
  }

  if (lid == 0) {
    /* Per-group results */
    float gmin_x = local_min_x[0];
    float gmax_x = local_max_x[0];
    float gmin_y = local_min_y[0];
    float gmax_y = local_max_y[0];
    float gmin_z = local_min_z[0];
    float gmax_z = local_max_z[0];

    /* Atomically accumulate into final_result (index 0 = sum, index 1 = count). */
    atomicAdd(final_result[0], local_sums[0]);
    atomicAdd(final_result[1], float(local_counts[0]));

    /* Update global min/max using ordered uint atomics (like SimpleDeform).
     * minmax_result layout: [minx, maxx, miny, maxy, minz, maxz]
     */
    if (gmin_x <= gmax_x && abs(gmin_x) < 1e30 && abs(gmax_x) < 1e30) {
      atomicMin(minmax_result[0], float_to_ordered_uint(gmin_x));
      atomicMax(minmax_result[1], float_to_ordered_uint(gmax_x));
    }
    if (gmin_y <= gmax_y && abs(gmin_y) < 1e30 && abs(gmax_y) < 1e30) {
      atomicMin(minmax_result[2], float_to_ordered_uint(gmin_y));
      atomicMax(minmax_result[3], float_to_ordered_uint(gmax_y));
    }
    if (gmin_z <= gmax_z && abs(gmin_z) < 1e30 && abs(gmax_z) < 1e30) {
      atomicMin(minmax_result[4], float_to_ordered_uint(gmin_z));
      atomicMax(minmax_result[5], float_to_ordered_uint(gmax_z));
    }
  }
}

)GLSL";

/* -------------------------------------------------------------------- */
/** \name Cast Compute Shader
 * \{ */

static const char *cast_compute_src = R"GLSL(
// MOD_cast flags (match DNA_modifier_types.h)
#define MOD_CAST_INVERT_VGROUP (1 << 0)
#define MOD_CAST_X (1 << 1)
#define MOD_CAST_Y (1 << 2)
#define MOD_CAST_Z (1 << 3)
#define MOD_CAST_USE_OB_TRANSFORM (1 << 4)
#define MOD_CAST_SIZE_FROM_RADIUS (1 << 5)

// Cast types
#define MOD_CAST_TYPE_SPHERE 0
#define MOD_CAST_TYPE_CYLINDER 1
#define MOD_CAST_TYPE_CUBOID 2

// Epsilon for float comparisons (match FLT_EPSILON)
#define FLT_EPSILON      1.192092896e-07F

// Minimal compute shader placeholder for MOD_cast (no-op passthrough).
// Will be replaced with full implementation later. The push-constants
// `fac`, `size`, `radius`, `flags`, `type`, `use_transform` are bound
// by the C++ dispatcher.
// Helper: normalize vec3 with safe threshold (matches CPU normalize_v3_v3_length)
float normalize_v3_v3_length(out vec3 r, vec3 a, float unit_length)
{
  float d = dot(a, a);

  /* A larger value causes normalize errors in a scaled down models with camera extreme close. */
  if (d > 1.0e-35) {
    d = sqrt(d);
    r = a * (unit_length / d);
  }
  else {
    /* Either the vector is small or one of it's values contained `nan`. */
    r = vec3(0.0);
    d = 0.0;
  }

  return d;
}

// Convenience wrapper using unit_length = 1.0
float normalize_v3_v3(out vec3 r, vec3 a)
{
  return normalize_v3_v3_length(r, a, 1.0);
}

/* Convert ordered uint back to float (reverse of float_to_ordered_uint from reduction shader) */
float ordered_uint_to_float(uint u) {
  uint f = (u & 0x80000000u) != 0u ? (u ^ 0x80000000u) : ~u;
  return uintBitsToFloat(f);
}

/* Read proj_len from SSBO final_result (sum, count) and compute average distance.
 * final_result[0] = sum, final_result[1] = count
 */
float proj_len_from_final()
{
  float sum = 0.0;
  float count = 0.0;
  /* final_result[] is bound as storage buffer by the C++ dispatcher */
  sum = final_result[0];
  count = final_result[1];
  if (count > 0.0) {
    return sum / count;
  }
  return 10.0;
}

/* Per-vertex cast for sphere/cylinder, mirrors CPU sphere_do. */
void sphere_do(uint v)
{
  vec4 co_in = input_positions[v];
  vec3 tmp_co = co_in.xyz;

  int flag = u_flags;
  int type_i = u_type; /* projection type */

  if (type_i == MOD_CAST_TYPE_CYLINDER) {
    flag &= ~MOD_CAST_Z;
  }

  bool has_ctrl_local = has_ctrl;
  mat4 mat = mat4(1.0);
  mat4 imat = mat4(1.0);
  vec3 center = vec3(0.0);

  if (has_ctrl_local) {
    if ((flag & MOD_CAST_USE_OB_TRANSFORM) != 0) {
      mat = inverse(ctrl_object_world) * object_world;
      imat = inverse(mat);
      tmp_co = (mat * vec4(tmp_co, 1.0)).xyz;
    }
    else {
      center = (inverse(object_world) * vec4(ctrl_object_world[3].xyz, 1.0)).xyz;
      tmp_co -= center;
    }
  }

  vec3 vec = tmp_co;
  if (type_i == MOD_CAST_TYPE_CYLINDER) {
    vec.z = 0.0;
  }

  bool has_radius = (radius > FLT_EPSILON);
  if (has_radius) {
    if (length(vec) > radius) {
      deformed_positions[v] = co_in;
      return;
    }
  }

  float fac_local = fac;
  float facm_local = 1.0 - fac_local;

  if (vgroup_weights.length() > 0 && v < vgroup_weights.length()) {
    float weight = vgroup_weights[v];
    if (weight == 0.0) {
      deformed_positions[v] = co_in;
      return;
    }
    fac_local = fac * weight;
    facm_local = 1.0 - fac_local;
  }

  /* Determine projection length */
  float len = ( (u_flags & MOD_CAST_SIZE_FROM_RADIUS) != 0 ) ? radius : size;
  if (len <= 0.0) {
    len = proj_len_from_final();
  }

  vec3 vec_n;
  normalize_v3_v3(vec_n, vec);

  if ((flag & MOD_CAST_X) != 0) {
    tmp_co.x = fac_local * vec_n.x * len + facm_local * tmp_co.x;
  }
  if ((flag & MOD_CAST_Y) != 0) {
    tmp_co.y = fac_local * vec_n.y * len + facm_local * tmp_co.y;
  }
  if ((flag & MOD_CAST_Z) != 0) {
    tmp_co.z = fac_local * vec_n.z * len + facm_local * tmp_co.z;
  }

  if (has_ctrl_local) {
    if ((flag & MOD_CAST_USE_OB_TRANSFORM) != 0) {
      tmp_co = (imat * vec4(tmp_co, 1.0)).xyz;
    }
    else {
      tmp_co += center;
    }
  }

  deformed_positions[v] = vec4(tmp_co, 1.0);
}

/* Per-vertex cuboid projection, ported from CPU cuboid_do. */
void cuboid_do(uint v)
{
  vec4 co_in = input_positions[v];
  vec3 tmp_co = co_in.xyz;

  int flag = u_flags;

  bool has_ctrl_local = has_ctrl;
  mat4 mat = mat4(1.0);
  mat4 imat = mat4(1.0);
  vec3 center = vec3(0.0);

  if (has_ctrl_local) {
    if ((flag & MOD_CAST_USE_OB_TRANSFORM) != 0) {
      mat = inverse(ctrl_object_world) * object_world;
      imat = inverse(mat);
    }
    else {
      center = (inverse(object_world) * vec4(ctrl_object_world[3].xyz, 1.0)).xyz;
    }
  }

  if (has_ctrl_local) {
    if ((flag & MOD_CAST_USE_OB_TRANSFORM) != 0) {
      tmp_co = (mat * vec4(tmp_co, 1.0)).xyz;
    }
    else {
      tmp_co -= center;
    }
  }

  bool has_radius = (radius > FLT_EPSILON);
  if (has_radius) {
    if (abs(tmp_co.x) > radius || abs(tmp_co.y) > radius || abs(tmp_co.z) > radius) {
      deformed_positions[v] = co_in;
      return;
    }
  }

  float fac_local = fac;
  float facm_local = 1.0 - fac_local;

  if (vgroup_weights.length() > 0 && v < vgroup_weights.length()) {
    float weight = vgroup_weights[v];
    if (weight == 0.0) {
      deformed_positions[v] = co_in;
      return;
    }
    fac_local = fac * weight;
    facm_local = 1.0 - fac_local;
  }

  /* compute bounding box min/max like CPU fallback.
   * If neither size nor radius provided, read global minmax_result computed by reduction.
   */
  vec3 minv, maxv;
  if ((flag & MOD_CAST_SIZE_FROM_RADIUS) != 0 && has_radius) {
    minv = vec3(-radius);
    maxv = vec3(radius);
  }
  else if ((flag & MOD_CAST_SIZE_FROM_RADIUS) == 0 && size > 0.0) {
    minv = vec3(-size);
    maxv = vec3(size);
  }
  else {
    /* try read minmax_result (ordered uints) */

    uint ux_minx = minmax_result[0];
    uint ux_maxx = minmax_result[1];
    uint ux_miny = minmax_result[2];
    uint ux_maxy = minmax_result[3];
    uint ux_minz = minmax_result[4];
    uint ux_maxz = minmax_result[5];

    float minx = ordered_uint_to_float(ux_minx);
    float maxx = ordered_uint_to_float(ux_maxx);
    float miny = ordered_uint_to_float(ux_miny);
    float maxy = ordered_uint_to_float(ux_maxy);
    float minz = ordered_uint_to_float(ux_minz);
    float maxz = ordered_uint_to_float(ux_maxz);

    /* Follow CPU logic: make symmetric bounds around origin like MOD_cast.c
     * Use min/max from reduction and mirror the largest absolute extent. */
    if (!(minx == minx && maxx == maxx && miny == miny && maxy == maxy && minz == minz && maxz == maxz)) {
      /* If reduction produced invalid values, derive a size from the average
       * projection length (like CPU uses proj_len when len<=0). */
      float fallback_len = proj_len_from_final();
      if (fallback_len <= 0.0) {
        fallback_len = 10.0;
      }
      minv = vec3(-fallback_len);
      maxv = vec3(fallback_len);
    }
    else {
      float ax = max(abs(minx), abs(maxx));
      float ay = max(abs(miny), abs(maxy));
      float az = max(abs(minz), abs(maxz));

      /* Make symmetric around origin following CPU: if abs(min) > fabs(max) use that. */
      if (abs(minx) > abs(maxx)) {
        maxx = abs(minx);
      }
      if (abs(miny) > abs(maxy)) {
        maxy = abs(miny);
      }
      if (abs(minz) > abs(maxz)) {
        maxz = abs(minz);
      }

      maxx = abs(maxx);
      maxy = abs(maxy);
      maxz = abs(maxz);

      minv = vec3(-maxx, -maxy, -maxz);
      maxv = vec3(maxx, maxy, maxz);
    }
  }

  /* find octant */
  int octant = 0;
  if (tmp_co.x > 0.0) octant += 1;
  if (tmp_co.y > 0.0) octant += 2;
  if (tmp_co.z > 0.0) octant += 4;

  vec3 apex;
  apex.x = (octant % 2 == 0) ? minv.x : maxv.x;
  apex.y = ((octant/2) % 2 == 0) ? minv.y : maxv.y;
  apex.z = (octant/4 == 0) ? minv.z : maxv.z;

  vec3 d;
  d.x = tmp_co.x / apex.x;
  d.y = tmp_co.y / apex.y;
  d.z = tmp_co.z / apex.z;

  float dmax = d.x;
  int coord = 0;
  if (d.y > dmax) { dmax = d.y; coord = 1; }
  if (d.z > dmax) { coord = 2; }

  if (abs(tmp_co[coord]) < FLT_EPSILON) {
    deformed_positions[v] = co_in;
    return;
  }

  float fbb = apex[coord] / tmp_co[coord];

  if ((flag & MOD_CAST_X) != 0) tmp_co.x = facm_local * tmp_co.x + fac_local * tmp_co.x * fbb;
  if ((flag & MOD_CAST_Y) != 0) tmp_co.y = facm_local * tmp_co.y + fac_local * tmp_co.y * fbb;
  if ((flag & MOD_CAST_Z) != 0) tmp_co.z = facm_local * tmp_co.z + fac_local * tmp_co.z * fbb;

  if (has_ctrl_local) {
    if ((flag & MOD_CAST_USE_OB_TRANSFORM) != 0) {
      tmp_co = (imat * vec4(tmp_co, 1.0)).xyz;
    }
    else {
      tmp_co += center;
    }
  }

  deformed_positions[v] = vec4(tmp_co, 1.0);
}

void main() {
  uint v = gl_GlobalInvocationID.x;
  if (v >= deformed_positions.length()) {
    return;
  }
  /* dispatch to appropriate projection type */
  if (u_type == MOD_CAST_TYPE_CUBOID) {
    cuboid_do(v);
  }
  else {
    /* compute proj_len from reduction SSBO (final_result) and let sphere_do use it */
    float proj_len = proj_len_from_final();
    //(void)proj_len; /* sphere_do may read final_result directly or use this value */
    sphere_do(v);
  }
}
)GLSL";

/** \} */

CastManager &CastManager::instance()
{
  static CastManager mgr;
  return mgr;
}

CastManager::CastManager() : impl_(new Impl()) {}
CastManager::~CastManager() {}

uint32_t CastManager::compute_cast_hash(const Mesh *mesh_orig, const CastModifierData *cmd)
{
  if (!mesh_orig || !cmd) {
    return 0;
  }

  uint32_t hash = 0;
  /* Vertex count */
  hash = BLI_hash_int_2d(hash, mesh_orig->verts_num);

  /* Control object (changes transform) */
  if (cmd->object) {
    hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(cmd->object)));
  }

  /* Vertex group name (affects weights) */
  if (cmd->defgrp_name[0] != '\0') {
    hash = BLI_hash_string(cmd->defgrp_name);
  }

  /* Modifier flags and type (size/from_radius/use_transform etc.) */
  hash = BLI_hash_int_2d(hash, int(cmd->type));
  hash = BLI_hash_int_2d(hash, int(cmd->flag));

  /* Deform verts pointer to detect weight array changes */
  blender::Span<MDeformVert> dverts = mesh_orig->deform_verts();
  hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(dverts.data())));

  return hash;
}

// ensure_static_resources: prepare per-mesh static data (vgroup weights) used by GPU dispatch.
void CastManager::ensure_static_resources(const CastModifierData *cmd,
                                         Object *ctrl_ob,
                                         Object *deformed_ob,
                                         Mesh *orig_mesh,
                                         uint32_t pipeline_hash)
{
  if (!orig_mesh || !cmd) {
    return;
  }

  Impl::MeshModifierKey key{orig_mesh, uint32_t(cmd->modifier.persistent_uid)};
  Impl::MeshStaticData &msd = impl_->static_map.lookup_or_add_default(key);

  const bool first_time = (msd.last_verified_hash == 0);
  const bool hash_changed = (pipeline_hash != msd.last_verified_hash);

  if (!first_time && !hash_changed) {
    return;
  }

  msd.last_verified_hash = pipeline_hash;
  msd.verts_num = orig_mesh->verts_num;
  msd.ctrl_ob = ctrl_ob;
  msd.deformed = deformed_ob;

  /* Extract vertex group weights if requested by modifier */
  msd.vgroup_weights.clear();
  if (cmd->defgrp_name[0] != '\0') {
    const int defgrp_index = BKE_id_defgroup_name_index(&orig_mesh->id, cmd->defgrp_name);
    if (defgrp_index != -1) {
      blender::Span<MDeformVert> dverts = orig_mesh->deform_verts();
      if (!dverts.is_empty()) {
        msd.vgroup_weights.resize(orig_mesh->verts_num, 0.0f);
        const bool invert_vgroup = (cmd->flag & MOD_CAST_INVERT_VGROUP) != 0;
        for (int v = 0; v < orig_mesh->verts_num; ++v) {
          const MDeformVert &dvert = dverts[v];
          float weight = BKE_defvert_find_weight(&dvert, defgrp_index);
          msd.vgroup_weights[v] = invert_vgroup ? 1.0f - weight : weight;
        }
      }
    }
  }
}

gpu::StorageBuf *CastManager::dispatch_deform(const CastModifierData *cmd,
                                              Depsgraph * /*depsgraph*/,
                                              Object *deformed_eval,
                                              MeshBatchCache *cache,
                                              gpu::StorageBuf *ssbo_in)
{
  if (!cmd || !deformed_eval || !ssbo_in) {
    return nullptr;
  }

  Mesh *mesh_owner = (cache && cache->mesh_owner) ? cache->mesh_owner : nullptr;
  if (!mesh_owner) {
    return nullptr;
  }

  Impl::MeshModifierKey key{mesh_owner, uint32_t(cmd->modifier.persistent_uid)};
  Impl::MeshStaticData *msd_ptr = impl_->static_map.lookup_ptr(key);
  if (!msd_ptr) {
    return nullptr;
  }
  Impl::MeshStaticData &msd = *msd_ptr;

  const std::string key_prefix = std::string("cast_") + std::to_string(key.hash()) + "_";
  const std::string key_vgroup = key_prefix + "vgroup_weights";
  const std::string key_out = key_prefix + "output";
  const std::string key_final = key_prefix + "final_reduction";
  const std::string key_minmax = key_prefix + "minmax";

  /* Ensure vgroup SSBO using helper (get -> ensure + upload when created). */
  gpu::StorageBuf *ssbo_vgroup = nullptr;
  ssbo_vgroup = modifier_gpu_helpers::ensure_vgroup_ssbo(
      mesh_owner, deformed_eval, key_vgroup, msd.vgroup_weights, msd.verts_num);

  /* Create output SSBO */
  const size_t size_out = msd.verts_num * sizeof(float) * 4;
  gpu::StorageBuf *ssbo_out = bke::BKE_mesh_gpu_internal_ssbo_ensure(
      mesh_owner, deformed_eval, key_out, size_out);
  if (!ssbo_out) {
    return nullptr;
  }

  /* Reduction buffers: per-workgroup sums and counts, size = num_groups */
  const int group_size = 256;
  const int num_groups = (msd.verts_num + group_size - 1) / group_size;
  /* Final reduction buffer: two floats (sum, count) */
  gpu::StorageBuf *ssbo_final = bke::BKE_mesh_gpu_internal_ssbo_ensure(
      mesh_owner, deformed_eval, key_final, 2 * sizeof(float));
  /* Min/max result SSBO (6 uints: minx,maxx,miny,maxy,minz,maxz) */
  gpu::StorageBuf *ssbo_minmax = bke::BKE_mesh_gpu_internal_ssbo_ensure(
      mesh_owner, deformed_eval, key_minmax, 6 * sizeof(uint32_t));

  /* Create/get shader */
  const std::string shader_key = "cast_compute";
  gpu::Shader *shader = bke::BKE_mesh_gpu_internal_shader_get(mesh_owner, shader_key);
  /* Reduction shader */
  const std::string shader_reduce_key = "cast_reduce";
  gpu::Shader *reduce_shader = bke::BKE_mesh_gpu_internal_shader_get(mesh_owner, shader_reduce_key);
  if (!reduce_shader) {
    using namespace gpu::shader;
    ShaderCreateInfo info_r("pyGPU_Shader");
    info_r.local_group_size(256, 1, 1);
    info_r.storage_buf(0, Qualifier::read, "vec4", "input_positions[]");
    info_r.storage_buf(1, Qualifier::write, "float", "final_result[]");
    info_r.storage_buf(2, Qualifier::write, "uint", "minmax_result[]");
    info_r.push_constant(Type::float4x4_t, "ctrl_object_world");
    info_r.push_constant(Type::float4x4_t, "object_world");
    info_r.push_constant(Type::bool_t, "has_ctrl");
    info_r.compute_source_generated = cast_reduction_src;
    reduce_shader = bke::BKE_mesh_gpu_internal_shader_ensure(mesh_owner, deformed_eval, shader_reduce_key, info_r);
  }
  if (!reduce_shader) {
    return nullptr;
  }

  /* reduction shader will directly write final_result via atomics */
  if (!shader) {
    using namespace gpu::shader;
    ShaderCreateInfo info("pyGPU_Shader");
    info.local_group_size(256, 1, 1);
    info.compute_source_generated = cast_compute_src;

    info.storage_buf(0, Qualifier::write, "vec4", "deformed_positions[]");
    info.storage_buf(1, Qualifier::read, "vec4", "input_positions[]");
    info.storage_buf(2, Qualifier::read, "float", "vgroup_weights[]");
    info.storage_buf(3, Qualifier::read, "float", "final_result[]");
    info.storage_buf(4, Qualifier::read, "uint", "minmax_result[]");

    /* push constants: factor, size, radius, flags, type, use_transform, ctrl matrix... */
    info.push_constant(Type::float_t, "fac");
    info.push_constant(Type::float_t, "size");
    info.push_constant(Type::float_t, "radius");
    info.push_constant(Type::int_t, "u_flags");
    info.push_constant(Type::int_t, "u_type");
    info.push_constant(Type::float4x4_t, "ctrl_object_world");
    info.push_constant(Type::float4x4_t, "object_world");
    info.push_constant(Type::float_t, "proj_len");
    info.push_constant(Type::bool_t, "has_ctrl");

    shader = bke::BKE_mesh_gpu_internal_shader_ensure(mesh_owner, deformed_eval, shader_key, info);
  }
  if (!shader) {
    return nullptr;
  }

  /* Pass object matrices to shader so math can be done on GPU (center, transforms). */
  float ctrl_world[4][4];
  float object_world[4][4];
  /* Prefer the evaluated control object stored in msd (set by ensure_static_resources).
   * Falls back to cmd->object if not available. */
  Object *ctrl_obj_for_shader = msd.ctrl_ob ? msd.ctrl_ob : cmd->object;
  if (ctrl_obj_for_shader) {
    copy_m4_m4(ctrl_world, ctrl_obj_for_shader->object_to_world().ptr());
  }
  else {
    unit_m4(ctrl_world);
  }
  if (deformed_eval) {
    copy_m4_m4(object_world, deformed_eval->object_to_world().ptr());
  }
  else {
    unit_m4(object_world);
  }

  /* Step 1: reduction pass to compute final_result (sum, count) via atomics */
  const gpu::shader::SpecializationConstants *constants_r = &GPU_shader_get_default_constant_state(reduce_shader);
  GPU_shader_bind(reduce_shader, constants_r);

  GPU_storagebuf_bind(ssbo_in, 0);
  GPU_storagebuf_bind(ssbo_final, 1);
  GPU_storagebuf_bind(ssbo_minmax, 2);

  /* Reset final_result to zeros */
  float init_final[2] = {0.0f, 0.0f};
  GPU_storagebuf_update(ssbo_final, init_final);
  /* Initialize minmax to ordered uint extremes (for 6 entries: minx,maxx,miny,maxy,minz,maxz) */
  uint32_t init_minmax[6] = {0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00000000u};
  GPU_storagebuf_update(ssbo_minmax, init_minmax);

  GPU_shader_uniform_mat4(reduce_shader, "ctrl_object_world", (const float(*)[4])ctrl_world);
  GPU_shader_uniform_mat4(reduce_shader, "object_world", (const float(*)[4])object_world);
  GPU_shader_uniform_1b(reduce_shader, "has_ctrl", msd.ctrl_ob != nullptr);

  GPU_compute_dispatch(reduce_shader, num_groups, 1, 1, constants_r);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  /* Step 2: main cast shader uses final_result (sum,count) to compute proj_len if needed */
  const gpu::shader::SpecializationConstants *constants = &GPU_shader_get_default_constant_state(shader);
  GPU_shader_bind(shader, constants);

  GPU_storagebuf_bind(ssbo_out, 0);
  GPU_storagebuf_bind(ssbo_in, 1);
  if (ssbo_vgroup) {
    GPU_storagebuf_bind(ssbo_vgroup, 2);
  }
  GPU_storagebuf_bind(ssbo_final, 3);
  GPU_storagebuf_bind(ssbo_minmax, 4);

  /* Set push constants from cmd */
  GPU_shader_uniform_1f(shader, "fac", cmd->fac);
  GPU_shader_uniform_1f(shader, "size", cmd->size);
  GPU_shader_uniform_1f(shader, "radius", cmd->radius);
  GPU_shader_uniform_1i(shader, "u_flags", int(cmd->flag));
  GPU_shader_uniform_1i(shader, "u_type", int(cmd->type));

  GPU_shader_uniform_mat4(shader, "ctrl_object_world", (const float(*)[4])ctrl_world);
  GPU_shader_uniform_mat4(shader, "object_world", (const float(*)[4])object_world);

  GPU_shader_uniform_1b(shader, "has_ctrl", msd.ctrl_ob != nullptr);

  GPU_compute_dispatch(shader, num_groups, 1, 1, constants);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  return ssbo_out;
}

void CastManager::free_resources_for_mesh(Mesh *mesh)
{
  if (!mesh) {
    return;
  }

  /* Remove all entries for this mesh (may be multiple Hook modifiers) */
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


void CastManager::invalidate_all(Mesh *mesh)
{
  if (!mesh) {
    return;
  }
  /* Free all GPU resources (SSBOs + shaders) for this mesh */
  bke::BKE_mesh_gpu_internal_resources_free_for_mesh(mesh);
}


void CastManager::free_all()
{
  impl_->static_map.clear();
}

}  // namespace draw
}  // namespace blender
