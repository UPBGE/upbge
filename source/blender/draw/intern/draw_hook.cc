/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_hook.hh"

#include "BLI_hash.h"
#include "BLI_map.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_vector.hh"

#include "BKE_action.hh"
#include "BKE_colortools.hh"
#include "BKE_deform.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_gpu.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"

#include "DNA_armature_types.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"

#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_storage_buffer.hh"

#include "../gpu/intern/gpu_shader_create_info.hh"

#include "DRW_render.hh"
#include "draw_cache_impl.hh"

#include "DEG_depsgraph_query.hh"

using namespace blender::draw;

/* -------------------------------------------------------------------- */
/** \name Internal Implementation Data
 * \{ */

struct blender::draw::HookManager::Impl {
  struct MeshStaticData {
    std::vector<float> vgroup_weights;      /* per-vertex weight (0.0-1.0) */
    std::vector<float> falloff_curve_lut;   /* curve falloff lookup table (256 samples) */
    int verts_num = 0;

    Object *hook_ob = nullptr;
    Object *deformed = nullptr;

    bool pending_gpu_setup = false;
    int gpu_setup_attempts = 0;
    uint32_t last_verified_hash = 0;
  };

  Map<Mesh *, MeshStaticData> static_map;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Hook Compute Shader (GPU port of MOD_hook.cc)
 * \{ */

static const char *hook_compute_src = R"GLSL(
/* Hook falloff types (matching HookModifierFalloff enum) */
#define HOOK_FALLOFF_NONE 0
#define HOOK_FALLOFF_CURVE 1
#define HOOK_FALLOFF_SHARP 2
#define HOOK_FALLOFF_SMOOTH 3
#define HOOK_FALLOFF_ROOT 4
#define HOOK_FALLOFF_LINEAR 5
#define HOOK_FALLOFF_CONST 6
#define HOOK_FALLOFF_SPHERE 7
#define HOOK_FALLOFF_INVSQUARE 8

/* Evaluate falloff curve using precomputed LUT */
float eval_curve_falloff(float t) {
  if (falloff_curve_lut.length() == 0) {
    return t;
  }
  int idx = int(clamp(t, 0.0, 1.0) * 255.0);
  return falloff_curve_lut[idx];
}

/* Compute hook falloff factor based on distance */
float hook_falloff_factor(float len_sq) {
  if (len_sq > falloff_sq) {
    return 0.0;
  }
  
  if (len_sq > 0.0) {
    float fac;
    
    if (falloff_type == HOOK_FALLOFF_CONST) {
      fac = 1.0;
      return fac * force;
    }
    else if (falloff_type == HOOK_FALLOFF_INVSQUARE) {
      fac = 1.0 - (len_sq / falloff_sq);
      return fac * force;
    }
    
    /* For other types, compute normalized distance */
    fac = 1.0 - (sqrt(len_sq) / falloff_radius);
    
    switch (falloff_type) {
      case HOOK_FALLOFF_CURVE:
        fac = eval_curve_falloff(fac);
        break;
      case HOOK_FALLOFF_SHARP:
        fac = fac * fac;
        break;
      case HOOK_FALLOFF_SMOOTH:
        fac = 3.0 * fac * fac - 2.0 * fac * fac * fac;
        break;
      case HOOK_FALLOFF_ROOT:
        fac = sqrt(fac);
        break;
      case HOOK_FALLOFF_LINEAR:
        /* Already linear, do nothing */
        break;
      case HOOK_FALLOFF_SPHERE:
        fac = sqrt(2.0 * fac - fac * fac);
        break;
    }
    
    return fac * force;
  }
  else {
    return force;
  }
}

void main() {
  uint v = gl_GlobalInvocationID.x;
  if (v >= deformed_positions.length()) {
    return;
  }

  vec4 co_in = input_positions[v];
  vec3 co = co_in.xyz;

  /* Get vertex group weight */
  float vgroup_weight = 1.0;
  if (vgroup_weights.length() > 0 && v < vgroup_weights.length()) {
    vgroup_weight = vgroup_weights[v];
  }

  /* Early exit if weight is negligible */
  if (vgroup_weight < 1e-6) {
    deformed_positions[v] = co_in;
    return;
  }

  /* Compute falloff factor based on distance */
  float fac;
  
  if (use_falloff) {
    float len_sq;
    
    if (use_uniform) {
      /* Transform to uniform space for distance calculation 
       * Note: mat_uniform is uploaded as mat4, but we only use upper-left 3x3 */
      vec3 co_uniform = (mat_uniform * vec4(co, 1.0)).xyz;
      len_sq = dot(hook_center - co_uniform, hook_center - co_uniform);
    }
    else {
      len_sq = dot(hook_center - co, hook_center - co);
    }
    
    fac = hook_falloff_factor(len_sq);
  }
  else {
    fac = force;
  }

  /* Apply hook transformation if factor is non-zero */
  if (fac > 1e-6) {
    fac *= vgroup_weight;
    
    if (fac > 1e-6) {
      /* Transform vertex to hook space */
      vec3 co_transformed = (hook_transform * vec4(co, 1.0)).xyz;
      
      /* Blend original and transformed position */
      co = mix(co, co_transformed, fac);
    }
  }

  deformed_positions[v] = vec4(co, 1.0);
}
)GLSL";

/** \} */

/* -------------------------------------------------------------------- */
/** \name HookManager Public API
 * \{ */

HookManager &HookManager::instance()
{
  static HookManager manager;
  return manager;
}

HookManager::HookManager() : impl_(new Impl()) {}
HookManager::~HookManager() {}

uint32_t HookManager::compute_hook_hash(const Mesh *mesh_orig, const HookModifierData *hmd)
{
  if (!mesh_orig || !hmd) {
    return 0;
  }

  uint32_t hash = 0;
  
  /* Hash vertex count */
  hash = BLI_hash_int_2d(hash, mesh_orig->verts_num);

  /* Hash hook object pointer */
  if (hmd->object) {
    hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(hmd->object)));
  }

  /* Hash subtarget bone name */
  if (hmd->subtarget[0] != '\0') {
    hash = BLI_hash_string(hmd->subtarget);
  }

  /* Hash falloff type */
  hash = BLI_hash_int_2d(hash, int(hmd->falloff_type));

  /* Hash flags */
  hash = BLI_hash_int_2d(hash, int(hmd->flag));

  /* Hash vertex group name */
  if (hmd->name[0] != '\0') {
    hash = BLI_hash_string(hmd->name);
  }

  /* Note: force, falloff, cent, parentinv are runtime uniforms, not hashed */

  return hash;
}

void HookManager::ensure_static_resources(const HookModifierData *hmd,
                                         Object *hook_ob,
                                         Object *deform_ob,
                                         Mesh *orig_mesh,
                                         uint32_t pipeline_hash)
{
  if (!orig_mesh || !hmd) {
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
  msd.hook_ob = hook_ob;
  msd.deformed = deform_ob;

  if (first_time || hash_changed) {
    msd.pending_gpu_setup = true;
    msd.gpu_setup_attempts = 0;
  }

  /* Extract vertex group weights */
  msd.vgroup_weights.clear();
  if (hmd->name[0] != '\0') {
    const int defgrp_index = BKE_id_defgroup_name_index(&orig_mesh->id, hmd->name);
    if (defgrp_index != -1) {
      blender::Span<MDeformVert> dverts = orig_mesh->deform_verts();
      if (!dverts.is_empty()) {
        msd.vgroup_weights.resize(orig_mesh->verts_num, 0.0f);
        const bool invert_vgroup = (hmd->flag & MOD_HOOK_INVERT_VGROUP) != 0;
        
        for (int v = 0; v < orig_mesh->verts_num; ++v) {
          const MDeformVert &dvert = dverts[v];
          float weight = BKE_defvert_find_weight(&dvert, defgrp_index);
          msd.vgroup_weights[v] = invert_vgroup ? 1.0f - weight : weight;
        }
      }
    }
  }

  /* Extract falloff curve LUT (256 samples) if using curve falloff */
  msd.falloff_curve_lut.clear();
  if (hmd->falloff_type == eHook_Falloff_Curve && hmd->curfalloff) {
    BKE_curvemapping_init(hmd->curfalloff);
    msd.falloff_curve_lut.resize(256);
    for (int i = 0; i < 256; i++) {
      float t = float(i) / 255.0f;
      msd.falloff_curve_lut[i] = BKE_curvemapping_evaluateF(hmd->curfalloff, 0, t);
    }
  }
}

blender::gpu::StorageBuf *HookManager::dispatch_deform(const HookModifierData *hmd,
                                                       Depsgraph * /*depsgraph*/,
                                                       Object *hook_ob_eval,
                                                       Object *deform_ob_eval,
                                                       MeshBatchCache *cache,
                                                       blender::gpu::StorageBuf *ssbo_in)
{
  if (!hmd || !hook_ob_eval) {
    return nullptr;
  }

  using namespace blender::draw;

  Mesh *mesh_owner = (cache && cache->mesh_owner) ? cache->mesh_owner : nullptr;
  if (!mesh_owner) {
    return nullptr;
  }

  Impl::MeshStaticData *msd_ptr = impl_->static_map.lookup_ptr(mesh_owner);
  if (!msd_ptr) {
    return nullptr;
  }
  Impl::MeshStaticData &msd = *msd_ptr;

  /* GPU setup retry logic */
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

  /* Upload vertex group weights SSBO */
  const std::string key_vgroup = "hook_vgroup_weights";
  blender::gpu::StorageBuf *ssbo_vgroup = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_vgroup);

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

  /* Upload falloff curve LUT SSBO (if using curve falloff) */
  const std::string key_curve = "hook_falloff_curve_lut";
  blender::gpu::StorageBuf *ssbo_curve = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_curve);

  if (!msd.falloff_curve_lut.empty()) {
    if (!ssbo_curve) {
      const size_t size_curve = msd.falloff_curve_lut.size() * sizeof(float);
      ssbo_curve = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_curve, size_curve);
      if (ssbo_curve) {
        GPU_storagebuf_update(ssbo_curve, msd.falloff_curve_lut.data());
      }
    }
  }
  else {
    /* No curve falloff: create empty dummy buffer (length=0 triggers default passthrough in shader) */
    if (!ssbo_curve) {
      ssbo_curve = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_curve, sizeof(float));
      if (ssbo_curve) {
        float dummy = 1.0f;  /* Unused, but set to 1.0 for safety */
        GPU_storagebuf_update(ssbo_curve, &dummy);
      }
    }
  }

  /* Create output SSBO */
  const std::string key_out = "hook_output";
  const size_t size_out = msd.verts_num * sizeof(float) * 4;
  blender::gpu::StorageBuf *ssbo_out = BKE_mesh_gpu_internal_ssbo_ensure(
      mesh_owner, key_out, size_out);
  if (!ssbo_out || !ssbo_in) {
    return nullptr;
  }

  /* Compute transformation matrices (same as CPU MOD_hook.cc) */
  float dmat[4][4];
  float hook_transform[4][4];
  float mat_uniform[3][3];
  float hook_center[3];

  /* Get hook target transform (object or bone) */
  if (hmd->subtarget[0] != '\0' && hook_ob_eval->pose) {
    bPoseChannel *pchan = BKE_pose_channel_find_name(hook_ob_eval->pose, hmd->subtarget);
    if (pchan) {
      /* Bone target */
      mul_m4_m4m4(dmat, hook_ob_eval->object_to_world().ptr(), pchan->pose_mat);
    }
    else {
      /* Bone not found, use object */
      copy_m4_m4(dmat, hook_ob_eval->object_to_world().ptr());
    }
  }
  else {
    /* Object target */
    copy_m4_m4(dmat, hook_ob_eval->object_to_world().ptr());
  }

  /* Compute final transformation: world_to_object * hook_world * parentinv
   * This transforms vertices from object space to hook space */
  float world_to_object[4][4];
  invert_m4_m4(world_to_object, deform_ob_eval->object_to_world().ptr());
  mul_m4_series(hook_transform, world_to_object, dmat, hmd->parentinv);

  /* Compute uniform space matrix and center (for falloff calculation) */
  const bool use_uniform = (hmd->flag & MOD_HOOK_UNIFORM_SPACE) != 0;
  if (use_uniform) {
    copy_m3_m4(mat_uniform, hmd->parentinv);
    mul_v3_m3v3(hook_center, mat_uniform, hmd->cent);
  }
  else {
    unit_m3(mat_uniform);
    copy_v3_v3(hook_center, hmd->cent);
  }

  /* Compute falloff parameters */
  const float falloff = (hmd->falloff_type == eHook_Falloff_None) ? 0.0f : hmd->falloff;
  const float falloff_sq = falloff * falloff;
  const bool use_falloff = (falloff_sq != 0.0f);

  /* Create shader */
  using namespace blender::gpu::shader;
  ShaderCreateInfo info("pyGPU_Shader");
  info.local_group_size(256, 1, 1);
  info.compute_source_generated = hook_compute_src;

  /* Bindings */
  info.storage_buf(0, Qualifier::write, "vec4", "deformed_positions[]");
  info.storage_buf(1, Qualifier::read, "vec4", "input_positions[]");
  info.storage_buf(2, Qualifier::read, "float", "vgroup_weights[]");
  info.storage_buf(3, Qualifier::read, "float", "falloff_curve_lut[]");

  /* Push constants */
  info.push_constant(Type::float4x4_t, "hook_transform");
  info.push_constant(Type::float4x4_t, "mat_uniform");  /* mat3 uploaded as mat4 */
  info.push_constant(Type::float3_t, "hook_center");
  info.push_constant(Type::float_t, "falloff_radius");
  info.push_constant(Type::float_t, "falloff_sq");
  info.push_constant(Type::float_t, "force");
  info.push_constant(Type::int_t, "falloff_type");
  info.push_constant(Type::bool_t, "use_falloff");
  info.push_constant(Type::bool_t, "use_uniform");

  blender::gpu::Shader *shader = BKE_mesh_gpu_internal_shader_ensure(
      mesh_owner, "hook_compute", info);
  if (!shader) {
    return nullptr;
  }

  /* Bind and dispatch */
  GPU_shader_bind(shader);

  GPU_storagebuf_bind(ssbo_out, 0);
  GPU_storagebuf_bind(ssbo_in, 1);
  if (ssbo_vgroup) {
    GPU_storagebuf_bind(ssbo_vgroup, 2);
  }
  if (ssbo_curve) {
    GPU_storagebuf_bind(ssbo_curve, 3);
  }

  /* Set uniforms */
  GPU_shader_uniform_mat4(shader, "hook_transform", (const float(*)[4])hook_transform);
  GPU_shader_uniform_mat3_as_mat4(shader, "mat_uniform", (const float(*)[3])mat_uniform);
  GPU_shader_uniform_3fv(shader, "hook_center", hook_center);
  GPU_shader_uniform_1f(shader, "falloff_radius", falloff);
  GPU_shader_uniform_1f(shader, "falloff_sq", falloff_sq);
  GPU_shader_uniform_1f(shader, "force", hmd->force);
  GPU_shader_uniform_1i(shader, "falloff_type", int(hmd->falloff_type));
  GPU_shader_uniform_1b(shader, "use_falloff", use_falloff);
  GPU_shader_uniform_1b(shader, "use_uniform", use_uniform);

  const int group_size = 256;
  const int num_groups = (msd.verts_num + group_size - 1) / group_size;
  GPU_compute_dispatch(shader, num_groups, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  msd.pending_gpu_setup = false;
  msd.gpu_setup_attempts = 0;

  return ssbo_out;
}

void HookManager::free_resources_for_mesh(Mesh *mesh)
{
  if (!mesh) {
    return;
  }
  impl_->static_map.remove(mesh);
}

void HookManager::invalidate_all(Mesh *mesh)
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

void HookManager::free_all()
{
  impl_->static_map.clear();
}

/** \} */
