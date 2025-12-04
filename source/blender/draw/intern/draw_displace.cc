/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * GPU-accelerated Displace modifier implementation.
 */

#include "draw_displace.hh"

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

#include "DEG_depsgraph_query.hh"

using namespace blender::draw;

/* -------------------------------------------------------------------- */
/** \name Internal Implementation Data
 * \{ */

struct blender::draw::DisplaceManager::Impl {
  /* Composite key: (Mesh*, modifier UID) to support multiple Displace modifiers per mesh */
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

    bool pending_gpu_setup = false;
    int gpu_setup_attempts = 0;
    uint32_t last_verified_hash = 0;
  };

  Map<MeshModifierKey, MeshStaticData> static_map;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Displace Compute Shader (GPU port of MOD_displace.cc)
 * \{ */

static const char *displace_compute_src = R"GLSL(
/* Displace direction modes (matching DisplaceModifierDirection enum) */
#define MOD_DISP_DIR_X 0
#define MOD_DISP_DIR_Y 1
#define MOD_DISP_DIR_Z 2
#define MOD_DISP_DIR_NOR 3
#define MOD_DISP_DIR_RGB_XYZ 4
#define MOD_DISP_DIR_CLNOR 5

/* Displace space modes (matching DisplaceModifierSpace enum) */
#define MOD_DISP_SPACE_LOCAL 0
#define MOD_DISP_SPACE_GLOBAL 1

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
  if (vgroup_weight < 0.0) {
    deformed_positions[v] = co_in;
    return;
  }

  /* Compute delta (displacement amount) */
  /* Note: For now, we use a fixed delta (no texture sampling) */
  float delta = (1.0 - midlevel) * strength * vgroup_weight;
  
  /* Clamp delta to prevent extreme deformations */
  delta = clamp(delta, -10000.0, 10000.0);

  /* Apply displacement based on direction */
  if (direction == MOD_DISP_DIR_X) {
    if (use_global) {
      /* Global X axis */
      co += delta * vec3(local_mat[0][0], local_mat[1][0], local_mat[2][0]);
    } else {
      /* Local X axis */
      co.x += delta;
    }
  }
  else if (direction == MOD_DISP_DIR_Y) {
    if (use_global) {
      /* Global Y axis */
      co += delta * vec3(local_mat[0][1], local_mat[1][1], local_mat[2][1]);
    } else {
      /* Local Y axis */
      co.y += delta;
    }
  }
  else if (direction == MOD_DISP_DIR_Z) {
    if (use_global) {
      /* Global Z axis */
      co += delta * vec3(local_mat[0][2], local_mat[1][2], local_mat[2][2]);
    } else {
      /* Local Z axis */
      co.z += delta;
    }
  }
  else if (direction == MOD_DISP_DIR_NOR) {
    /* Displacement along vertex normal */
    vec3 normal = vert_normals[v].xyz;
    co += delta * normal;
  }
  /* Note: MOD_DISP_DIR_RGB_XYZ and MOD_DISP_DIR_CLNOR not supported yet */

  deformed_positions[v] = vec4(co, 1.0);
}
)GLSL";

/** \} */

/* -------------------------------------------------------------------- */
/** \name DisplaceManager Public API
 * \{ */

DisplaceManager &DisplaceManager::instance()
{
  static DisplaceManager manager;
  return manager;
}

DisplaceManager::DisplaceManager() : impl_(new Impl()) {}
DisplaceManager::~DisplaceManager() {}

uint32_t DisplaceManager::compute_displace_hash(const Mesh *mesh_orig,
                                                const DisplaceModifierData *dmd)
{
  if (!mesh_orig || !dmd) {
    return 0;
  }

  uint32_t hash = 0;

  /* Hash vertex count */
  hash = BLI_hash_int_2d(hash, mesh_orig->verts_num);

  /* Hash direction mode */
  hash = BLI_hash_int_2d(hash, int(dmd->direction));

  /* Hash space mode */
  hash = BLI_hash_int_2d(hash, int(dmd->space));

  /* Hash vertex group name */
  if (dmd->defgrp_name[0] != '\0') {
    hash = BLI_hash_string(dmd->defgrp_name);
  }

  /* Hash invert flag */
  hash = BLI_hash_int_2d(hash, int(dmd->flag & MOD_DISP_INVERT_VGROUP));

  /* Note: strength and midlevel are runtime uniforms, not hashed */

  return hash;
}

void DisplaceManager::ensure_static_resources(const DisplaceModifierData *dmd,
                                             Object *deform_ob,
                                             Mesh *orig_mesh,
                                             uint32_t pipeline_hash)
{
  if (!orig_mesh || !dmd) {
    return;
  }

  /* Use composite key (mesh, modifier_uid) to support multiple Displace modifiers per mesh */
  Impl::MeshModifierKey key{orig_mesh, uint32_t(dmd->modifier.persistent_uid)};
  Impl::MeshStaticData &msd = impl_->static_map.lookup_or_add_default(key);

  const bool first_time = (msd.last_verified_hash == 0);
  const bool hash_changed = (pipeline_hash != msd.last_verified_hash);
  const bool gpu_invalidated = msd.pending_gpu_setup;

  if (!first_time && !hash_changed && !gpu_invalidated) {
    return;
  }

  msd.last_verified_hash = pipeline_hash;
  msd.verts_num = orig_mesh->verts_num;
  msd.deformed = deform_ob;

  if (first_time || hash_changed) {
    msd.pending_gpu_setup = true;
    msd.gpu_setup_attempts = 0;
  }

  /* Extract vertex group weights */
  msd.vgroup_weights.clear();
  if (dmd->defgrp_name[0] != '\0') {
    const int defgrp_index = BKE_id_defgroup_name_index(&orig_mesh->id, dmd->defgrp_name);
    if (defgrp_index != -1) {
      blender::Span<MDeformVert> dverts = orig_mesh->deform_verts();
      if (!dverts.is_empty()) {
        msd.vgroup_weights.resize(orig_mesh->verts_num, 0.0f);
        const bool invert_vgroup = (dmd->flag & MOD_DISP_INVERT_VGROUP) != 0;

        for (int v = 0; v < orig_mesh->verts_num; ++v) {
          const MDeformVert &dvert = dverts[v];
          float weight = BKE_defvert_find_weight(&dvert, defgrp_index);
          msd.vgroup_weights[v] = invert_vgroup ? 1.0f - weight : weight;
        }
      }
    }
  }
}

blender::gpu::StorageBuf *DisplaceManager::dispatch_deform(const DisplaceModifierData *dmd,
                                                           Depsgraph * /*depsgraph*/,
                                                           Object *deformed_eval,
                                                           MeshBatchCache *cache,
                                                           blender::gpu::StorageBuf *ssbo_in)
{
  if (!dmd) {
    return nullptr;
  }

  using namespace blender::draw;

  Mesh *mesh_owner = (cache && cache->mesh_owner) ? cache->mesh_owner : nullptr;
  if (!mesh_owner) {
    return nullptr;
  }

  /* Use composite key (mesh, modifier_uid) to support multiple Displace modifiers per mesh */
  Impl::MeshModifierKey key{mesh_owner, uint32_t(dmd->modifier.persistent_uid)};
  Impl::MeshStaticData *msd_ptr = impl_->static_map.lookup_ptr(key);
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

  /* Create unique buffer keys per modifier instance using persistent_uid */
  char uid_str[16];
  snprintf(uid_str, sizeof(uid_str), "%u", dmd->modifier.persistent_uid);
  const std::string key_prefix = std::string("displace_") + uid_str + "_";
  const std::string key_vgroup = key_prefix + "vgroup_weights";
  const std::string key_normals = key_prefix + "vert_normals";
  const std::string key_out = key_prefix + "output";

  /* Upload vertex group weights SSBO */
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
    /* No vertex group: create dummy buffer (length=0 triggers default weight=1.0 in shader) */
    if (!ssbo_vgroup) {
      ssbo_vgroup = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_vgroup, sizeof(float));
      if (ssbo_vgroup) {
        float dummy = 1.0f;
        GPU_storagebuf_update(ssbo_vgroup, &dummy);
      }
    }
  }

  /* Upload vertex normals SSBO (only 1 time - See if we can improve later) */
  /* NOTE: Vertex normals are NOT recalculated after GPU deformation, so MOD_DISP_DIR_NOR
   * will use ORIGINAL normals (before any deformation). This is a known limitation.
   * For accurate normal-based displacement after GPU deformation, use CPU fallback.
   * TODO: Integrate with scatter shader's recalculated normals. */
  blender::gpu::StorageBuf *ssbo_normals = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_normals);
  
  if (!ssbo_normals) {
    /* Extract vertex normals from mesh_owner (ORIGINAL, not deformed) */
    const blender::Span<blender::float3> normals = mesh_owner->vert_normals();
    if (!normals.is_empty()) {
      const size_t size_normals = normals.size() * sizeof(blender::float3);
      ssbo_normals = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_normals, size_normals);
      if (ssbo_normals) {
        GPU_storagebuf_update(ssbo_normals, normals.data());
      }
    }
    else {
      /* No normals available: create dummy buffer pointing up (Z axis) */
      ssbo_normals = BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, key_normals, sizeof(blender::float4));
      if (ssbo_normals) {
        blender::float4 dummy(0.0f, 0.0f, 1.0f, 0.0f);  /* Z-up as dummy */
        GPU_storagebuf_update(ssbo_normals, &dummy);
      }
    }
  }

  /* Create output SSBO */
  const size_t size_out = msd.verts_num * sizeof(float) * 4;
  blender::gpu::StorageBuf *ssbo_out = BKE_mesh_gpu_internal_ssbo_ensure(
      mesh_owner, key_out, size_out);
  if (!ssbo_out || !ssbo_in) {
    return nullptr;
  }

  /* Compute transformation matrix (for global space) */
  float local_mat[4][4];
  const bool use_global = (dmd->space == MOD_DISP_SPACE_GLOBAL);
  if (use_global) {
    copy_m4_m4(local_mat, deformed_eval->object_to_world().ptr());
  }
  else {
    unit_m4(local_mat);
  }

  /* Create shader */
  using namespace blender::gpu::shader;
  ShaderCreateInfo info("pyGPU_Shader");
  info.local_group_size(256, 1, 1);
  info.compute_source_generated = displace_compute_src;

  /* Bindings */
  info.storage_buf(0, Qualifier::write, "vec4", "deformed_positions[]");
  info.storage_buf(1, Qualifier::read, "vec4", "input_positions[]");
  info.storage_buf(2, Qualifier::read, "float", "vgroup_weights[]");
  info.storage_buf(3, Qualifier::read, "vec4", "vert_normals[]");  // Always declare (dummy if unused)

  /* Push constants */
  info.push_constant(Type::float4x4_t, "local_mat");
  info.push_constant(Type::float_t, "strength");
  info.push_constant(Type::float_t, "midlevel");
  info.push_constant(Type::int_t, "direction");
  info.push_constant(Type::bool_t, "use_global");

  blender::gpu::Shader *shader = BKE_mesh_gpu_internal_shader_ensure(
      mesh_owner, "displace_compute", info);
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
  if (ssbo_normals) {
    GPU_storagebuf_bind(ssbo_normals, 3);  // Always bind (dummy if unused)
  }

  /* Set uniforms (runtime parameters) */
  GPU_shader_uniform_mat4(shader, "local_mat", (const float(*)[4])local_mat);
  GPU_shader_uniform_1f(shader, "strength", dmd->strength);
  GPU_shader_uniform_1f(shader, "midlevel", dmd->midlevel);
  GPU_shader_uniform_1i(shader, "direction", int(dmd->direction));
  GPU_shader_uniform_1b(shader, "use_global", use_global);

  const int group_size = 256;
  const int num_groups = (msd.verts_num + group_size - 1) / group_size;
  GPU_compute_dispatch(shader, num_groups, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  msd.pending_gpu_setup = false;
  msd.gpu_setup_attempts = 0;

  return ssbo_out;
}

void DisplaceManager::free_resources_for_mesh(Mesh *mesh)
{
  if (!mesh) {
    return;
  }

  /* Remove all entries for this mesh (may be multiple Displace modifiers) */
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

void DisplaceManager::invalidate_all(Mesh *mesh)
{
  if (!mesh) {
    return;
  }

  BKE_mesh_gpu_internal_resources_free_for_mesh(mesh);

  /* Invalidate all Displace modifiers for this mesh */
  for (auto item : impl_->static_map.items()) {
    if (item.key.mesh == mesh) {
      /* Lookup again to get mutable reference */
      Impl::MeshStaticData *msd = impl_->static_map.lookup_ptr(item.key);
      if (msd) {
        msd->pending_gpu_setup = true;
        msd->gpu_setup_attempts = 0;
      }
    }
  }
}

void DisplaceManager::free_all()
{
  impl_->static_map.clear();
}

/** \} */
