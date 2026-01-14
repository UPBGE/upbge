/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_lattice_deform.hh"

#include "BLI_hash.h"
#include "BLI_map.hh"
#include "BLI_math_matrix.h"
#include "BLI_vector.hh"

#include "BKE_curve.hh"
#include "BKE_deform.hh"  // For BKE_defvert_find_weight, BKE_id_defgroup_name_index
#include "BKE_lattice.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_gpu.hh"
#include "BKE_modifier.hh"  // For BKE_modifiers_is_deformed_by_lattice
#include "BKE_object.hh"

#include "DNA_lattice_types.h"
#include "DNA_mesh_types.h"  // For MDeformVert
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

struct blender::draw::LatticeSkinningManager::Impl {
/* Composite key: (Mesh*, modifier UID) to support multiple Lattice modifiers per mesh */
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
    std::vector<float> control_points; /* float3 per control point */
    std::vector<float> vgroup_weights; /* per-vertex weight (0.0-1.0) */
    int verts_num = 0;
    int lattice_u = 0, lattice_v = 0, lattice_w = 0;

    Object *lattice = nullptr;
    Object *deformed = nullptr;
    uint32_t last_verified_hash = 0;
  };

  Map<MeshModifierKey, MeshStaticData> static_map;
};

/* Lattice deformation compute shader (GPU port of BKE_lattice_deform_data_eval_co) */
static const char *lattice_compute_src = R"GLSL(
/* Bezier/Linear interpolation weights (same as key_curve_position_weights in BKE_key.h) */
void calc_curve_weights(float t, int type, out float weights[4]) {
  if (type == 1) { /* KEY_LINEAR */
    weights[0] = 0.0;
    weights[1] = 1.0 - t;
    weights[2] = t;
    weights[3] = 0.0;
  }
  else { /* KEY_BSPLINE (default) */
    float t2 = t * t;
    float t3 = t2 * t;
    weights[0] = -0.16666667 * t3 + 0.5 * t2 - 0.5 * t + 0.16666667;
    weights[1] = 0.5 * t3 - t2 + 0.66666667;
    weights[2] = -0.5 * t3 + 0.5 * t2 + 0.5 * t + 0.16666667;
    weights[3] = 0.16666667 * t3;
  }
}

void main() {
  uint v = gl_GlobalInvocationID.x;
  if (v >= deformed_positions.length()) {
    return;
  }

  vec4 co = input_positions[v];
  vec3 co_orig = co.xyz;

  /* Get per-vertex weight from vertex group (defaults to 1.0 if no vgroup) */
  float vgroup_weight = 1.0;
  if (vgroup_weights.length() > 0 && v < vgroup_weights.length()) {
    vgroup_weight = vgroup_weights[v];
  }

  /* Global modifier strength */
  float modifier_weight = strength * vgroup_weight;

  /* Early exit if weight is negligible */
  if (modifier_weight < 1e-6) {
    deformed_positions[v] = co;
    return;
  }

  /* Transform to lattice space (same as CPU: mul_v3_m4v3(vec, latmat, co)) */
  vec3 vec = (latmat[0] * co).xyz;

  /* Compute UVW coordinates */
  float u, v_coord, w;
  int ui, vi, wi;
  float tu[4], tv[4], tw[4];

  /* U axis */
  if (lattice_dims.x > 1.0) {
    u = (vec.x - lattice_origin.x) / lattice_spacing.x;
    ui = int(floor(u));
    u -= float(ui);
    calc_curve_weights(u, lattice_types.x, tu);
  } else {
    tu[0] = tu[2] = tu[3] = 0.0;
    tu[1] = 1.0;
    ui = 0;
  }

  /* V axis */
  if (lattice_dims.y > 1.0) {
    v_coord = (vec.y - lattice_origin.y) / lattice_spacing.y;
    vi = int(floor(v_coord));
    v_coord -= float(vi);
    calc_curve_weights(v_coord, lattice_types.y, tv);
  } else {
    tv[0] = tv[2] = tv[3] = 0.0;
    tv[1] = 1.0;
    vi = 0;
  }

  /* W axis */
  if (lattice_dims.z > 1.0) {
    w = (vec.z - lattice_origin.z) / lattice_spacing.z;
    wi = int(floor(w));
    w -= float(wi);
    calc_curve_weights(w, lattice_types.z, tw);
  } else {
    tw[0] = tw[2] = tw[3] = 0.0;
    tw[1] = 1.0;
    wi = 0;
  }

  /* Strides for indexing control points */
  int w_stride = int(lattice_dims.x) * int(lattice_dims.y);
  int v_stride = int(lattice_dims.x);
  int idx_w_max = (int(lattice_dims.z) - 1) * w_stride;
  int idx_v_max = (int(lattice_dims.y) - 1) * v_stride;
  int idx_u_max = int(lattice_dims.x) - 1;

  /* 4x4x4 interpolation (64 control points) */
  vec3 deformed = vec3(0.0);

  for (int ww = wi - 1; ww <= wi + 2; ww++) {
    float ww_weight = modifier_weight * tw[ww - wi + 1];
    int idx_w = clamp(ww * w_stride, 0, idx_w_max);

    for (int vv = vi - 1; vv <= vi + 2; vv++) {
      float vv_weight = ww_weight * tv[vv - vi + 1];
      int idx_v = clamp(vv * v_stride, 0, idx_v_max);

      for (int uu = ui - 1; uu <= ui + 2; uu++) {
        float uu_weight = vv_weight * tu[uu - ui + 1];
        int idx_u = clamp(uu, 0, idx_u_max);
        int idx = idx_w + idx_v + idx_u;

        /* Accumulate weighted control point deltas */
        vec3 cp_delta = vec3(
          control_points[idx * 3 + 0],
          control_points[idx * 3 + 1],
          control_points[idx * 3 + 2]
        );
        deformed += cp_delta * uu_weight;
      }
    }
  }
  /* Final deformed position */
  deformed_positions[v] = vec4(co_orig + deformed, 1.0);
}
)GLSL";

LatticeSkinningManager &LatticeSkinningManager::instance()
{
  static LatticeSkinningManager manager;
  return manager;
}

LatticeSkinningManager::LatticeSkinningManager() : impl_(new Impl()) {}
LatticeSkinningManager::~LatticeSkinningManager() {}

uint32_t LatticeSkinningManager::compute_lattice_hash(const Mesh *mesh_orig,
                                                      const LatticeModifierData *lmd)
{
  if (!mesh_orig || !lmd) {
    return 0;
  }

  uint32_t hash = 0;
  hash = BLI_hash_int_2d(hash, mesh_orig->verts_num);

  /* Hash lattice object pointer */
  if (lmd->object) {
    hash = BLI_hash_int_2d(hash, (int)(intptr_t)lmd->object);

    /* Hash lattice dimensions and control point count */
    if (lmd->object->data) {
      Lattice *lt = id_cast<Lattice *>(lmd->object->data);
      hash = BLI_hash_int_2d(hash, lt->pntsu);
      hash = BLI_hash_int_2d(hash, lt->pntsv);
      hash = BLI_hash_int_2d(hash, lt->pntsw);

      /* Hash interpolation types (KEY_LINEAR vs KEY_BSPLINE) */
      hash = BLI_hash_int_2d(hash, int(lt->typeu));
      hash = BLI_hash_int_2d(hash, int(lt->typev));
      hash = BLI_hash_int_2d(hash, int(lt->typew));
    }
  }

  /* Hash vertex group name (if specified) */
  if (lmd->name[0] != '\0') {
    hash = BLI_hash_string(lmd->name);
  }

  /* Hash deform_verts pointer (detects vertex group changes) */
  blender::Span<MDeformVert> dverts = mesh_orig->deform_verts();
  hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(dverts.data())));

  /* NOTE: strength is NOT hashed (it's a runtime uniform, changes every frame) */

  return hash;
}

void LatticeSkinningManager::ensure_static_resources(const LatticeModifierData *lmd,
                                                     Object *lattice_ob,
                                                     Object *deformed_ob,
                                                     Mesh *orig_mesh,
                                                     uint32_t pipeline_hash)
{
  if (!orig_mesh || !lattice_ob || !lmd) {
    return;
  }

  /* Use composite key (mesh, modifier_uid) to support multiple Lattice modifiers per mesh */
  Impl::MeshModifierKey key{orig_mesh, uint32_t(lmd->modifier.persistent_uid)};
  Impl::MeshStaticData &msd = impl_->static_map.lookup_or_add_default(key);

  const bool first_time = (msd.last_verified_hash == 0);
  const bool hash_changed = (pipeline_hash != msd.last_verified_hash);

  if (!first_time && !hash_changed) {
    return;
  }

  msd.last_verified_hash = pipeline_hash;
  msd.verts_num = orig_mesh->verts_num;
  msd.lattice = lattice_ob;
  msd.deformed = deformed_ob;

  /* Extract lattice control points (same as CPU version) */
  Lattice *lt = BKE_object_get_lattice(lattice_ob);
  if (!lt) {
    return;
  }

  msd.lattice_u = lt->pntsu;
  msd.lattice_v = lt->pntsv;
  msd.lattice_w = lt->pntsw;

  const int num_points = lt->pntsu * lt->pntsv * lt->pntsw;
  msd.control_points.resize(num_points * 3);

  /* Compute transformation matrices (same as BKE_lattice_deform_data_create) */
  float latmat[4][4], imat[4][4];
  if (deformed_ob) {
    invert_m4_m4(imat, lattice_ob->object_to_world().ptr());
    mul_m4_m4m4(latmat, imat, deformed_ob->object_to_world().ptr());
    invert_m4_m4(imat, latmat);
  }
  else {
    invert_m4_m4(latmat, lattice_ob->object_to_world().ptr());
    invert_m4_m4(imat, latmat);
  }

  /* Extract and transform control points */
  BPoint *bp = lt->def;
  float *fp = msd.control_points.data();

  for (int w = 0; w < lt->pntsw; w++) {
    float fw = lt->fw + w * lt->dw;
    for (int v = 0; v < lt->pntsv; v++) {
      float fv = lt->fv + v * lt->dv;
      for (int u = 0; u < lt->pntsu; u++, fp += 3, bp++) {
        float fu = lt->fu + u * lt->du;
        fp[0] = bp->vec[0] - fu;
        fp[1] = bp->vec[1] - fv;
        fp[2] = bp->vec[2] - fw;
        mul_mat3_m4_v3(imat, fp);
      }
    }
  }

  /* Extract vertex group weights from mesh (now using lmd directly!) */
  msd.vgroup_weights.clear();
  if (lmd->name[0] != '\0') {
    /* Find vertex group index in mesh */
    const int defgrp_index = BKE_id_defgroup_name_index(&orig_mesh->id, lmd->name);
    if (defgrp_index != -1) {
      /* Extract per-vertex weights */
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

gpu::StorageBuf *LatticeSkinningManager::dispatch_deform(
    const LatticeModifierData *lmd,
    Depsgraph * /*depsgraph*/,
    Object *eval_lattice,
    Object *deformed_eval,
    MeshBatchCache *cache,
    gpu::StorageBuf *ssbo_in)
{
  if (!lmd || !ssbo_in) {
    return nullptr;
  }

  Mesh *mesh_owner = (cache && cache->mesh_owner) ? cache->mesh_owner : nullptr;
  if (!mesh_owner || !eval_lattice) {
    return nullptr;
  }

  /* Use composite key (mesh, modifier_uid) to support multiple Lattice modifiers per mesh */
  Impl::MeshModifierKey key{mesh_owner, uint32_t(lmd->modifier.persistent_uid)};
  Impl::MeshStaticData *msd_ptr = impl_->static_map.lookup_ptr(key);
  if (!msd_ptr) {
    return nullptr;
  }
  Impl::MeshStaticData &msd = *msd_ptr;

  /* Create unique buffer keys per modifier instance using composite key hash
   * to avoid collisions when multiple Lattice modifiers are on the same mesh */
  const std::string key_prefix = "lattice_" + std::to_string(key.hash()) + "_";
  const std::string key_cp = key_prefix + "control_points";
  const std::string key_mat = key_prefix + "latmat";
  const std::string key_vgroup = key_prefix + "vgroup_weights";
  const std::string key_out = key_prefix + "output";

  /* Control points SSBO */
  gpu::StorageBuf *ssbo_cp = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_cp);
  if (!ssbo_cp && !msd.control_points.empty()) {
    const size_t size_cp = msd.control_points.size() * sizeof(float);
    ssbo_cp = bke::BKE_mesh_gpu_internal_ssbo_ensure(mesh_owner, deformed_eval, key_cp, size_cp);
    if (ssbo_cp) {
      GPU_storagebuf_update(ssbo_cp, msd.control_points.data());
    }
  }

  /* Transformation matrix SSBO */
  gpu::StorageBuf *ssbo_mat = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_mat);
  if (!ssbo_mat) {
    float latmat[4][4];
    if (deformed_eval) {
      float imat[4][4];
      invert_m4_m4(imat, eval_lattice->object_to_world().ptr());
      mul_m4_m4m4(latmat, imat, deformed_eval->object_to_world().ptr());
    }
    else {
      invert_m4_m4(latmat, eval_lattice->object_to_world().ptr());
    }
    ssbo_mat = bke::BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, deformed_eval, key_mat, sizeof(float) * 16);
    if (ssbo_mat) {
      GPU_storagebuf_update(ssbo_mat, &latmat[0][0]);
    }
  }

  if (!ssbo_cp || !ssbo_mat) {
    return nullptr;
  }

  /* Update latmat every frame (lattice/mesh may have moved) */
  {
    float latmat[4][4];
    if (deformed_eval) {
      float imat[4][4];
      invert_m4_m4(imat, eval_lattice->object_to_world().ptr());
      mul_m4_m4m4(latmat, imat, deformed_eval->object_to_world().ptr());
    }
    else {
      invert_m4_m4(latmat, eval_lattice->object_to_world().ptr());
    }
    GPU_storagebuf_update(ssbo_mat, &latmat[0][0]);
  }

  /* Update control points every frame (lattice may be animated) */
  if (!msd.control_points.empty()) {
    Lattice *lt = BKE_object_get_lattice(eval_lattice);
    if (lt && lt->def) {
      /* Recompute transformation for control points */
      float latmat[4][4], imat[4][4];
      if (deformed_eval) {
        invert_m4_m4(imat, eval_lattice->object_to_world().ptr());
        mul_m4_m4m4(latmat, imat, deformed_eval->object_to_world().ptr());
        invert_m4_m4(imat, latmat);
      }
      else {
        invert_m4_m4(latmat, eval_lattice->object_to_world().ptr());
        invert_m4_m4(imat, latmat);
      }

      BPoint *bp = lt->def;
      float *fp = msd.control_points.data();

      for (int w = 0; w < lt->pntsw; w++) {
        float fw = lt->fw + w * lt->dw;
        for (int v = 0; v < lt->pntsv; v++) {
          float fv = lt->fv + v * lt->dv;
          for (int u = 0; u < lt->pntsu; u++, fp += 3, bp++) {
            float fu = lt->fu + u * lt->du;
            fp[0] = bp->vec[0] - fu;
            fp[1] = bp->vec[1] - fv;
            fp[2] = bp->vec[2] - fw;
            mul_mat3_m4_v3(imat, fp);
          }
        }
      }
      GPU_storagebuf_update(ssbo_cp, msd.control_points.data());
    }
  }

  /* Create output SSBO */
  const size_t size_out = msd.verts_num * sizeof(float) * 4;
  gpu::StorageBuf *ssbo_out = bke::BKE_mesh_gpu_internal_ssbo_ensure(
      mesh_owner, deformed_eval, key_out, size_out);
  if (!ssbo_out) {
    return nullptr;
  }

  /* Create shader */
  const std::string shader_key = "lattice_deform";
  gpu::Shader *shader = bke::BKE_mesh_gpu_internal_shader_get(mesh_owner, shader_key);
  if (!shader) {
    using namespace gpu::shader;
    ShaderCreateInfo info("pyGPU_Shader");
    info.local_group_size(256, 1, 1);
    info.compute_source_generated = lattice_compute_src;

    /* Bindings */
    info.storage_buf(0, Qualifier::write, "vec4", "deformed_positions[]");
    info.storage_buf(1, Qualifier::read, "vec4", "input_positions[]");
    info.storage_buf(2, Qualifier::read, "float", "control_points[]");
    info.storage_buf(3, Qualifier::read, "mat4", "latmat[]");
    info.storage_buf(4, Qualifier::read, "float", "vgroup_weights[]");  // Optional vertex group

    /* Push constants (uniforms) - correct enum types */
    info.push_constant(Type::float3_t, "lattice_dims");     // vec3 (float3_t)
    info.push_constant(Type::float3_t, "lattice_origin");   // vec3 (float3_t)
    info.push_constant(Type::float3_t, "lattice_spacing");  // vec3 (float3_t)
    info.push_constant(Type::int3_t, "lattice_types");      // ivec3 (int3_t)
    info.push_constant(Type::float_t, "strength");          // float (modifier strength)

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
  GPU_storagebuf_bind(ssbo_cp, 2);
  GPU_storagebuf_bind(ssbo_mat, 3);

  /* Bind vertex group weights SSBO at binding=4 */
  gpu::StorageBuf *ssbo_vgroup = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_owner,
                                                                              key_vgroup);

  /* Only create/upload if vertex group weights exist */
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
    /* No vertex group: create per-vertex buffer filled with 1.0f. This avoids
     * issues where a single-float dummy leads to incorrect reads on some
     * backends. If mesh has zero vertices, allocate single float. */
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

  if (ssbo_vgroup) {
    GPU_storagebuf_bind(ssbo_vgroup, 4);
  }

  Lattice *lt = BKE_object_get_lattice(eval_lattice);
  if (!lt) {
    return nullptr;
  }

  /* Set push constants */
  GPU_shader_uniform_3f(
      shader, "lattice_dims", float(lt->pntsu), float(lt->pntsv), float(lt->pntsw));
  GPU_shader_uniform_3f(shader, "lattice_origin", lt->fu, lt->fv, lt->fw);
  GPU_shader_uniform_3f(shader, "lattice_spacing", lt->du, lt->dv, lt->dw);

  /* Set lattice_types as ivec3 using GPU_shader_uniform_3iv */
  const int types[3] = {int(lt->typeu), int(lt->typev), int(lt->typew)};
  GPU_shader_uniform_3iv(shader, "lattice_types", types);

  /* Extract strength from LatticeModifierData (runtime uniform, not hashed) */
  const float strength = lmd->strength;
  GPU_shader_uniform_1f(shader, "strength", strength);

  const int group_size = 256;
  const int num_groups = (msd.verts_num + group_size - 1) / group_size;
  GPU_compute_dispatch(shader, num_groups, 1, 1, constants);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  return ssbo_out;
}

void LatticeSkinningManager::free_resources_for_mesh(Mesh *mesh)
{
  if (!mesh) {
    return;
  }
  
  /* Remove all entries for this mesh (may be multiple Lattice modifiers) */
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

void LatticeSkinningManager::invalidate_all(Mesh *mesh)
{
  if (!mesh) {
    return;
  }
  /* Free all GPU resources (SSBOs + shaders) for this mesh */
  bke::BKE_mesh_gpu_internal_resources_free_for_mesh(mesh);
}

void LatticeSkinningManager::free_all()
{
  impl_->static_map.clear();
}

}  // namespace draw
}  // namespace blender
