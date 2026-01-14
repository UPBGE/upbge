/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_simpledeform.hh"

#include "BLI_hash.h"
#include "BLI_map.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_vector.hh"

#include "BKE_deform.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_gpu.hh"
#include "BKE_object.hh"

#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"

#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_storage_buffer.hh"

#include "../gpu/intern/gpu_shader_create_info.hh"

#include "DRW_render.hh"
#include "draw_cache_impl.hh"
#include "draw_cache_extract.hh"

namespace blender {
namespace draw {

struct blender::draw::SimpleDeformManager::Impl {
/* Composite key: (Mesh*, modifier UID) to support multiple SimpleDeform modifiers per mesh */
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
    std::vector<float> vgroup_weights; /* per-vertex weight (0.0-1.0) */
    int verts_num = 0;

    Object *deformed = nullptr;
    uint32_t last_verified_hash = 0;
  };

  blender::Map<MeshModifierKey, MeshStaticData> static_map;
};

/* Min/Max reduction compute shader (finds lower/upper bounds along limit_axis) */
static const char *minmax_reduction_src = R"GLSL(
/* Per-workgroup reduction then single atomic update per group into minmax_result[].
 * Based on BGE Armature bounds reduction pattern. */

uint float_to_ordered_uint(float f) {
  uint u = floatBitsToUint(f);
  return (u & 0x80000000u) != 0u ? ~u : (u ^ 0x80000000u);
}

shared float local_min_vals[256];
shared float local_max_vals[256];

void main() {
  const uint gid = gl_GlobalInvocationID.x;
  const uint lid = gl_LocalInvocationID.x;
  const uint group_size = gl_WorkGroupSize.x;
  const uint num_verts = input_positions.length();
  const uint stride = group_size * gl_NumWorkGroups.x;

  /* Per-thread local min/max */
  float tmin =  1.0/0.0;  /* +INF */
  float tmax = -1.0/0.0;  /* -INF */

  /* Each thread processes a strided subset of vertices */
  for (uint i = gid; i < num_verts; i += stride) {
    vec3 pos = input_positions[i].xyz;
    
    /* Transform to deform space */
    pos = (transmat * vec4(pos, 1.0)).xyz;
    
    /* Extract coordinate along limit_axis */
    float val = pos[limit_axis];
    
    /* Check if finite */
    if (val == val && abs(val) < 1e30) {  /* NaN check and range check */
      tmin = min(tmin, val);
      tmax = max(tmax, val);
    }
  }

  /* Store into shared memory */
  local_min_vals[lid] = tmin;
  local_max_vals[lid] = tmax;

  barrier();
  memoryBarrierShared();

  /* Parallel reduction in shared memory */
  for (uint s = group_size >> 1; s > 0; s >>= 1) {
    if (lid < s) {
      local_min_vals[lid] = min(local_min_vals[lid], local_min_vals[lid + s]);
      local_max_vals[lid] = max(local_max_vals[lid], local_max_vals[lid + s]);
    }
    barrier();
    memoryBarrierShared();
  }

  /* Single thread updates the global final bounds (few atomics per group) */
  if (lid == 0) {
    float gmin = local_min_vals[0];
    float gmax = local_max_vals[0];

    /* Ignore empty group */
    if (gmin <= gmax && abs(gmin) < 1e30 && abs(gmax) < 1e30) {
      atomicMin(minmax_result[0], float_to_ordered_uint(gmin));
      atomicMax(minmax_result[1], float_to_ordered_uint(gmax));
    }
  }
}
)GLSL";

/* Simple Deform compute shader (GPU port of MOD_simpledeform.cc) */
static const char *simpledeform_compute_src = R"GLSL(
#define MOD_SIMPLEDEFORM_MODE_TWIST 1
#define MOD_SIMPLEDEFORM_MODE_BEND 2
#define MOD_SIMPLEDEFORM_MODE_TAPER 3
#define MOD_SIMPLEDEFORM_MODE_STRETCH 4

#define MOD_SIMPLEDEFORM_LOCK_AXIS_X (1 << 0)
#define MOD_SIMPLEDEFORM_LOCK_AXIS_Y (1 << 1)
#define MOD_SIMPLEDEFORM_LOCK_AXIS_Z (1 << 2)

#define BEND_EPS 0.000001

/* Convert ordered uint back to float (reverse of float_to_ordered_uint from reduction shader) */
float ordered_uint_to_float(uint u) {
  uint f = (u & 0x80000000u) != 0u ? (u ^ 0x80000000u) : ~u;
  return uintBitsToFloat(f);
}

/* Axis remapping table (same as CPU axis_map_table) */
const int axis_map[3][3] = int[3][3](
  int[3](1, 2, 0),  // X axis
  int[3](2, 0, 1),  // Y axis
  int[3](0, 1, 2)   // Z axis
);

/* Remap vector using axis map */
vec3 remap_axis_vec(vec3 v, int axis_idx) {
  int map_x = axis_map[axis_idx][0];
  int map_y = axis_map[axis_idx][1];
  int map_z = axis_map[axis_idx][2];
  return vec3(v[map_x], v[map_y], v[map_z]);
}

/* Unmap vector back to original axes */
vec3 unmap_axis_vec(vec3 v, int axis_idx) {
  vec3 result;
  int map_x = axis_map[axis_idx][0];
  int map_y = axis_map[axis_idx][1];
  int map_z = axis_map[axis_idx][2];
  result[map_x] = v.x;
  result[map_y] = v.y;
  result[map_z] = v.z;
  return result;
}

/* Clamp axis (same as CPU axis_limit) */
void axis_limit_gpu(int axis, vec2 limits, inout vec3 co, inout vec3 dcut) {
  float val = co[axis];
  val = clamp(val, limits.x, limits.y);
  dcut[axis] = co[axis] - val;
  co[axis] = val;
}

/* Transform coordinate to deform space */
vec3 simpledeform_transform_in(vec3 co) {
  return (transmat * vec4(co, 1.0)).xyz;
}

/* Transform coordinate back from deform space */
vec3 simpledeform_transform_out(vec3 co) {
  return (transmat_inv * vec4(co, 1.0)).xyz;
}

/* Twist deform (same as CPU simpleDeform_twist) */
vec3 simpledeform_twist(vec3 co, float factor, vec3 dcut) {
  float x = co.x, y = co.y, z = co.z;
  float theta = z * factor;
  float sint = sin(theta);
  float cost = cos(theta);
  
  vec3 r_co;
  r_co.x = x * cost - y * sint;
  r_co.y = x * sint + y * cost;
  r_co.z = z;
  
  return r_co + dcut;
}

/* Bend deform (same as CPU simpleDeform_bend) */
vec3 simpledeform_bend(vec3 co, float factor, int axis, vec3 dcut) {
  float x = co.x, y = co.y, z = co.z;
  float theta;
  
  if (abs(factor) < BEND_EPS) {
    return co + dcut;
  }
  
  if (axis == 0 || axis == 1) {
    theta = z * factor;
  } else {
    theta = x * factor;
  }
  
  float sint = sin(theta);
  float cost = cos(theta);
  
  vec3 r_co;
  if (axis == 0) {
    r_co.x = x;
    r_co.y = y * cost + (1.0 - cost) / factor;
    r_co.z = -(y - 1.0 / factor) * sint;
    r_co.x += dcut.x;
    r_co.y += sint * dcut.z;
    r_co.z += cost * dcut.z;
  }
  else if (axis == 1) {
    r_co.x = x * cost + (1.0 - cost) / factor;
    r_co.y = y;
    r_co.z = -(x - 1.0 / factor) * sint;
    r_co.x += sint * dcut.z;
    r_co.y += dcut.y;
    r_co.z += cost * dcut.z;
  }
  else {
    r_co.x = -(y - 1.0 / factor) * sint;
    r_co.y = y * cost + (1.0 - cost) / factor;
    r_co.z = z;
    r_co.x += cost * dcut.x;
    r_co.y += sint * dcut.x;
    r_co.z += dcut.z;
  }
  
  return r_co;
}

/* Taper deform (same as CPU simpleDeform_taper) */
vec3 simpledeform_taper(vec3 co, float factor, vec3 dcut) {
  float x = co.x, y = co.y, z = co.z;
  float scale = z * factor;
  
  vec3 r_co;
  r_co.x = x + x * scale;
  r_co.y = y + y * scale;
  r_co.z = z;
  
  return r_co + dcut;
}

/* Stretch deform (same as CPU simpleDeform_stretch) */
vec3 simpledeform_stretch(vec3 co, float factor, vec3 dcut) {
  float x = co.x, y = co.y, z = co.z;
  float scale = (z * z * factor - factor + 1.0);
  
  vec3 r_co;
  r_co.x = x * scale;
  r_co.y = y * scale;
  r_co.z = z * (1.0 + factor);
  
  return r_co + dcut;
}

void main() {
uint v = gl_GlobalInvocationID.x;
if (v >= deformed_positions.length()) {
  return;
}

/* Read computed min/max from reduction pass (avoid CPU readback!) */
float lower = ordered_uint_to_float(minmax_bounds[0]);
float upper = ordered_uint_to_float(minmax_bounds[1]);
  
/* Calculate absolute limits */
float smd_limit_lower = lower + (upper - lower) * limit_lower_factor;
float smd_limit_upper = lower + (upper - lower) * limit_upper_factor;
  
/* Calculate normalized factor */
float smd_factor = raw_factor / max(1e-10, smd_limit_upper - smd_limit_lower);

vec4 co_in = input_positions[v];
vec3 vertexCo = co_in.xyz;

/* Get modifier vertex group weight */
  float weight = 1.0;
  if (vgroup_weights.length() > 0 && v < vgroup_weights.length()) {
    weight = vgroup_weights[v];
  }

  /* Early exit if weight is negligible */
  if (weight < 1e-6) {
    deformed_positions[v] = co_in;
    return;
  }

  /* Transform vertexCo to deform space (same as CPU: modifies vertexCo in-place) */
  vertexCo = simpledeform_transform_in(vertexCo);
  
  /* Copy to co for deformation (same as CPU: copy_v3_v3(co, vertexCos[iter])) */
  vec3 co = vertexCo;
  vec3 dcut = vec3(0.0);
  
  /* Apply axis locks (same as CPU) */
  const vec2 base_limit = vec2(0.0, 0.0);
  if ((lock_axis & MOD_SIMPLEDEFORM_LOCK_AXIS_X) != 0) {
    axis_limit_gpu(0, base_limit, co, dcut);
  }
  if ((lock_axis & MOD_SIMPLEDEFORM_LOCK_AXIS_Y) != 0) {
    axis_limit_gpu(1, base_limit, co, dcut);
  }
  if ((lock_axis & MOD_SIMPLEDEFORM_LOCK_AXIS_Z) != 0) {
    axis_limit_gpu(2, base_limit, co, dcut);
  }
  
  /* Apply limit axis clamp (use computed limits from reduction) */
  axis_limit_gpu(limit_axis, vec2(smd_limit_lower, smd_limit_upper), co, dcut);
  
  /* Determine which axis to use for remapping */
  int remap_axis_idx = (deform_mode == MOD_SIMPLEDEFORM_MODE_BEND) ? 2 : deform_axis;
  
  /* Remap co and dcut */
  vec3 co_remap = remap_axis_vec(co, remap_axis_idx);
  vec3 dcut_remap = remap_axis_vec(dcut, remap_axis_idx);
  
  /* Apply deformation (use computed smd_factor from GPU bounds) */
  vec3 co_deformed;
  if (deform_mode == MOD_SIMPLEDEFORM_MODE_TWIST) {
    co_deformed = simpledeform_twist(co_remap, smd_factor, dcut_remap);
  }
  else if (deform_mode == MOD_SIMPLEDEFORM_MODE_BEND) {
    co_deformed = simpledeform_bend(co_remap, smd_factor, deform_axis, dcut_remap);
  }
  else if (deform_mode == MOD_SIMPLEDEFORM_MODE_TAPER) {
    co_deformed = simpledeform_taper(co_remap, smd_factor, dcut_remap);
  }
  else if (deform_mode == MOD_SIMPLEDEFORM_MODE_STRETCH) {
    co_deformed = simpledeform_stretch(co_remap, smd_factor, dcut_remap);
  }
  else {
    co_deformed = co_remap;
  }
  
  /* Unmap back */
  co = unmap_axis_vec(co_deformed, remap_axis_idx);
  
  /* Blend vertexCo with deformed co based on weight (same as CPU: interp_v3_v3v3) */
  vertexCo = mix(vertexCo, co, weight);
  
  /* Transform back to world space (same as CPU: BLI_space_transform_invert on vertexCos[iter]) */
  vertexCo = simpledeform_transform_out(vertexCo);
  
  deformed_positions[v] = vec4(vertexCo, 1.0);
}
)GLSL";

SimpleDeformManager &SimpleDeformManager::instance()
{
  static SimpleDeformManager manager;
  return manager;
}

SimpleDeformManager::SimpleDeformManager() : impl_(new Impl()) {}
SimpleDeformManager::~SimpleDeformManager() {}

uint32_t SimpleDeformManager::compute_simpledeform_hash(
    const Mesh *mesh_orig, const blender::SimpleDeformModifierData *smd)
{
  if (!mesh_orig || !smd) {
    return 0;
  }

  uint32_t hash = 0;
  hash = BLI_hash_int_2d(hash, mesh_orig->verts_num);

  /* Hash mode */
  hash = BLI_hash_int_2d(hash, int(smd->mode));

  /* Hash axis */
  hash = BLI_hash_int_2d(hash, int(smd->axis));

  /* Hash origin object pointer (if specified) */
  if (smd->origin) {
    hash = BLI_hash_int_2d(hash, (int)(intptr_t)smd->origin);
  }

  /* Hash vertex group name (if specified) */
  if (smd->vgroup_name[0] != '\0') {
    hash = BLI_hash_string(smd->vgroup_name);
  }

  /* Hash deform_verts pointer (detects vertex group changes) */
  blender::Span<MDeformVert> dverts = mesh_orig->deform_verts();
  hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(dverts.data())));

  /* NOTE: factor is NOT hashed (it's a runtime uniform) */

  return hash;
}

void SimpleDeformManager::ensure_static_resources(const blender::SimpleDeformModifierData *smd,
                                                  Object *deform_ob,
                                                  Mesh *orig_mesh,
                                                  uint32_t pipeline_hash)
{
  if (!orig_mesh || !smd) {
    return;
  }

  /* Use composite key (mesh, modifier_uid) to support multiple SimpleDeform modifiers per mesh */
  Impl::MeshModifierKey key{orig_mesh, uint32_t(smd->modifier.persistent_uid)};
  Impl::MeshStaticData &msd = impl_->static_map.lookup_or_add_default(key);

  const bool first_time = (msd.last_verified_hash == 0);
  const bool hash_changed = (pipeline_hash != msd.last_verified_hash);

  if (!first_time && !hash_changed) {
    return;
  }

  msd.last_verified_hash = pipeline_hash;
  msd.verts_num = orig_mesh->verts_num;
  msd.deformed = deform_ob;

  /* Extract vertex group weights from mesh */
  msd.vgroup_weights.clear();
  if (smd->vgroup_name[0] != '\0') {
    const int defgrp_index = BKE_id_defgroup_name_index(&orig_mesh->id, smd->vgroup_name);
    if (defgrp_index != -1) {
      blender::Span<MDeformVert> dverts = orig_mesh->deform_verts();

      /* Check if dverts is empty to prevent crash
       * When ALL vertex groups are deleted, dverts.data() == nullptr.
       * Accessing dverts[v] would crash with Access Violation. */
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

gpu::StorageBuf *SimpleDeformManager::dispatch_deform(
    const blender::SimpleDeformModifierData *smd,
    Depsgraph * /*depsgraph*/,
    Object *deformed_eval,
    MeshBatchCache *cache,
    gpu::StorageBuf *ssbo_in)
{
  if (!smd || !ssbo_in) {
    return nullptr;
  }

  Mesh *mesh_owner = (cache && cache->mesh_owner) ? cache->mesh_owner : nullptr;
  if (!mesh_owner) {
    return nullptr;
  }

  /* Use composite key (mesh, modifier_uid) to support multiple SimpleDeform modifiers per mesh */
  Impl::MeshModifierKey key{mesh_owner, uint32_t(smd->modifier.persistent_uid)};
  Impl::MeshStaticData *msd_ptr = impl_->static_map.lookup_ptr(key);
  if (!msd_ptr) {
    return nullptr;
  }
  Impl::MeshStaticData &msd = *msd_ptr;

  /* Create unique buffer keys per modifier instance using composite key hash
   * to avoid collisions when multiple SimpleDeform modifiers are on the same mesh */
  const std::string key_prefix = "simpledeform_" + std::to_string(key.hash()) + "_";
  const std::string key_vgroup = key_prefix + "vgroup_weights";
  const std::string key_out = key_prefix + "output";

  /* Vertex group weights SSBO */
  gpu::StorageBuf *ssbo_vgroup = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_vgroup);

  if (!msd.vgroup_weights.empty()) {
    if (!ssbo_vgroup) {
      const size_t size_vgroup = msd.vgroup_weights.size() * sizeof(float);
      ssbo_vgroup = bke::BKE_mesh_gpu_internal_ssbo_ensure(
          mesh_owner, deformed_eval, key_vgroup, size_vgroup);
      if (ssbo_vgroup) {
        GPU_storagebuf_update(ssbo_vgroup, msd.vgroup_weights.data());
      }
    }
  }
  else {
    /* No vertex group selected: create a per-vertex buffer filled with 1.0f.
     * This avoids backend-dependent behavior when using a single-float
     * dummy (which can lead to incorrect reads on OpenGL). If the mesh has
     * zero vertices allocate a single float to satisfy minimum buffer size. */
    if (!ssbo_vgroup) {
      const size_t count = (msd.verts_num > 0) ? size_t(msd.verts_num) : size_t(1);
      const size_t size_vgroup = count * sizeof(float);
      ssbo_vgroup = bke::BKE_mesh_gpu_internal_ssbo_ensure(
          mesh_owner, deformed_eval, key_vgroup, size_vgroup);
      if (ssbo_vgroup) {
        std::vector<float> dummy(count, 1.0f);
        GPU_storagebuf_update(ssbo_vgroup, dummy.data());
      }
    }
  }

  /* Create output SSBO */
  const size_t size_out = msd.verts_num * sizeof(float) * 4;
  gpu::StorageBuf *ssbo_out = bke::BKE_mesh_gpu_internal_ssbo_ensure(
      mesh_owner, deformed_eval, key_out, size_out);
  if (!ssbo_out) {
    return nullptr;
  }

  /* Compute transformation matrices (same as CPU: BLI_SPACE_TRANSFORM_SETUP)
   * transmat transforms from object space to deform space (origin)
   * CPU does: transmat = inverse(origin) * object */
  float transmat[4][4], transmat_inv[4][4];

  if (smd->origin) {
    /* Transform: object → origin space */
    float origin_imat[4][4];
    invert_m4_m4(origin_imat, smd->origin->object_to_world().ptr());
    mul_m4_m4m4(transmat, origin_imat, deformed_eval->object_to_world().ptr());
  }
  else {
    /* No origin: identity transform */
    unit_m4(transmat);
  }

  invert_m4_m4(transmat_inv, transmat);

  /* Calculate lock_axis and limit_axis (same as CPU MOD_simpledeform.cc) */
  const int deform_axis = std::clamp(int(smd->deform_axis), 0, 2);
  int lock_axis = smd->axis;
  int limit_axis = deform_axis;

  if (smd->mode == MOD_SIMPLEDEFORM_MODE_BEND) {
    lock_axis = 0; /* Bend doesn't use lock axis */
    /* Bend limit_axis is special */
    if (deform_axis == 0 || deform_axis == 1) {
      limit_axis = 2;
    }
    else {
      limit_axis = 0;
    }
  }
  else {
    /* Don't lock the deform axis */
    if (deform_axis == 0) {
      lock_axis &= ~MOD_SIMPLEDEFORM_LOCK_AXIS_X;
    }
    if (deform_axis == 1) {
      lock_axis &= ~MOD_SIMPLEDEFORM_LOCK_AXIS_Y;
    }
    if (deform_axis == 2) {
      lock_axis &= ~MOD_SIMPLEDEFORM_LOCK_AXIS_Z;
    }
  }

  /* Calculate absolute limits and smd_factor using GPU reduction pass
   * This ensures we use DEFORMED positions (ssbo_in), not REST positions! */

  /* Create min/max result SSBO (2 uints for atomic operations) */
  const std::string key_minmax = "simpledeform_minmax";
  const size_t size_minmax = 2 * sizeof(uint32_t);
  gpu::StorageBuf *ssbo_minmax = bke::BKE_mesh_gpu_internal_ssbo_ensure(
      mesh_owner, deformed_eval, key_minmax, size_minmax);
  if (!ssbo_minmax) {
    return nullptr;
  }

  /* Initialize min/max to extreme values (as ordered uints) */
  uint32_t init_minmax[2] = {0xFFFFFFFFu, 0x00000000u}; /* max uint, min uint */
  GPU_storagebuf_update(ssbo_minmax, init_minmax);

  /* Create reduction shader */
  const std::string shader_min_max_key = "simpledeform_minmax";
  gpu::Shader *minmax_shader = bke::BKE_mesh_gpu_internal_shader_get(mesh_owner,
                                                                         shader_min_max_key);
  if (!minmax_shader) {
    using namespace gpu::shader;
    ShaderCreateInfo minmax_info("pyGPU_Shader");
    minmax_info.local_group_size(256, 1, 1);
    minmax_info.storage_buf(0, Qualifier::read, "vec4", "input_positions[]");
    minmax_info.storage_buf(1, Qualifier::write, "uint", "minmax_result[]");
    minmax_info.push_constant(Type::float4x4_t, "transmat");
    minmax_info.push_constant(Type::int_t, "limit_axis");
    minmax_info.compute_source_generated = minmax_reduction_src;

    minmax_shader = bke::BKE_mesh_gpu_internal_shader_ensure(
        mesh_owner, deformed_eval, shader_min_max_key, minmax_info);
  }
  if (!minmax_shader) {
    return nullptr;
  }

  /* Dispatch reduction pass */
  GPU_shader_bind(minmax_shader);
  GPU_storagebuf_bind(ssbo_in, 0);
  GPU_storagebuf_bind(ssbo_minmax, 1);
  GPU_shader_uniform_mat4(minmax_shader, "transmat", (const float(*)[4])transmat);
  GPU_shader_uniform_1i(minmax_shader, "limit_axis", limit_axis);

  const int minmax_groups = (msd.verts_num + 255) / 256;
  GPU_compute_dispatch(minmax_shader, minmax_groups, 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  /* Note: No CPU readback! The minmax results stay on GPU and are read by the deform shader */

  /* Create shader */
  const std::string shader_key = "simpledeform";
  gpu::Shader *shader = bke::BKE_mesh_gpu_internal_shader_get(mesh_owner, shader_key);
  if (!shader) {
    using namespace gpu::shader;
    ShaderCreateInfo info("pyGPU_Shader");
    info.local_group_size(256, 1, 1);
    info.compute_source_generated = simpledeform_compute_src;

    /* Bindings */
    info.storage_buf(0, Qualifier::write, "vec4", "deformed_positions[]");
    info.storage_buf(1, Qualifier::read, "vec4", "input_positions[]");
    info.storage_buf(2, Qualifier::read, "float", "vgroup_weights[]");
    info.storage_buf(
        3, Qualifier::read, "uint", "minmax_bounds[]"); /* Computed by reduction pass */

    /* Push constants */
    info.push_constant(Type::float4x4_t, "transmat");
    info.push_constant(Type::float4x4_t, "transmat_inv");
    info.push_constant(Type::int_t, "deform_mode");
    info.push_constant(Type::int_t, "deform_axis");
    info.push_constant(Type::int_t, "lock_axis");
    info.push_constant(Type::int_t, "limit_axis");
    info.push_constant(Type::float_t, "raw_factor");         /* Raw modifier factor */
    info.push_constant(Type::float_t, "limit_lower_factor"); /* smd->limit[0] */
    info.push_constant(Type::float_t, "limit_upper_factor"); /* smd->limit[1] */

    shader = bke::BKE_mesh_gpu_internal_shader_ensure(mesh_owner, deformed_eval, shader_key, info);
  }
  if (!shader) {
    return nullptr;
  }

  /* Bind and dispatch */
  const gpu::shader::SpecializationConstants *constants =
      &GPU_shader_get_default_constant_state(shader);
  GPU_shader_bind(shader, constants);

  GPU_storagebuf_bind(ssbo_out, 0);
  GPU_storagebuf_bind(ssbo_in, 1);
  if (ssbo_vgroup) {
    GPU_storagebuf_bind(ssbo_vgroup, 2);
  }
  GPU_storagebuf_bind(ssbo_minmax, 3); /* Bind minmax results from reduction pass */

  /* Set uniforms */
  GPU_shader_uniform_mat4(shader, "transmat", (const float(*)[4])transmat);
  GPU_shader_uniform_mat4(shader, "transmat_inv", (const float(*)[4])transmat_inv);
  GPU_shader_uniform_1i(shader, "deform_mode", int(smd->mode));
  GPU_shader_uniform_1i(shader, "deform_axis", deform_axis);
  GPU_shader_uniform_1i(shader, "lock_axis", lock_axis);
  GPU_shader_uniform_1i(shader, "limit_axis", limit_axis);
  GPU_shader_uniform_1f(shader, "raw_factor", smd->factor);           /* Raw factor */
  GPU_shader_uniform_1f(shader, "limit_lower_factor", smd->limit[0]); /* Normalized lower */
  GPU_shader_uniform_1f(shader, "limit_upper_factor", smd->limit[1]); /* Normalized upper */

  const int group_size = 256;
  const int num_groups = (msd.verts_num + group_size - 1) / group_size;
  GPU_compute_dispatch(shader, num_groups, 1, 1, constants);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  return ssbo_out;
}

void SimpleDeformManager::free_resources_for_mesh(Mesh *mesh)
{
  if (!mesh) {
    return;
  }

  /* Remove all entries for this mesh (may be multiple SimpleDeform modifiers) */
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

void SimpleDeformManager::invalidate_all(Mesh *mesh)
{
  if (!mesh) {
    return;
  }
  /* Free all GPU resources (SSBOs + shaders) for this mesh */
  bke::BKE_mesh_gpu_internal_resources_free_for_mesh(mesh);
}

void SimpleDeformManager::free_all()
{
  impl_->static_map.clear();
}

}  // namespace draw
}  // namespace blender
