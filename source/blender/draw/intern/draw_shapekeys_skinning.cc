/* SPDX-FileCopyrightText:2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ShapeKey GPU blending manager (squelette)
 */

#include "draw_shapekeys_skinning.hh"

#include <map>
#include <string>
#include <vector>

#include "BLI_map.hh"
#include "BLI_threads.h"
#include "BLI_vector.hh"

#include "BKE_key.hh"
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

#include "DEG_depsgraph_query.hh"
#include "DNA_mesh_types.h"

using namespace blender::draw;

struct blender::draw::ShapeKeySkinningManager::Impl {
  struct MeshStaticData {
    std::vector<float> rest_positions; /* vec4 per vert */
    std::vector<float> deltas;         /* flattened: key_idx * verts * 4 */
    int verts_num = 0;
    int key_count = 0; /* excluding basis */
    bool pending_gpu_setup = false;
    int gpu_setup_attempts = 0;
  };

  Map<Mesh *, MeshStaticData> static_map;
};

static const char *shapekey_compute_src = R"GLSL(
/* compute shader: out_pos[v] = rest_pos[v] + sum_k weights[k] * deltas[k*V + v] */
//uniform int u_vert_count;
//uniform int u_key_count;
//layout(std430, binding = 0) buffer RestPos { vec4 rest_pos[]; };
//layout(std430, binding = 1) buffer Deltas { vec4 deltas[]; }; /* flattened: k * V + v */
//layout(std430, binding = 2) buffer Weights { float weights[]; }; /* per key */
//layout(std430, binding = 3) buffer OutPos { vec4 out_pos[]; };

void main() {
  uint v = gl_GlobalInvocationID.x;
  if (int(v) >= u_vert_count) return;
  vec4 p = rest_pos[v];
  for (int k = 0; k < u_key_count; ++k) {
    float w = weights[k];
    if (abs(w) > 1e-6) {
      uint idx = uint(k) * uint(u_vert_count) + v;
      p += deltas[idx] * w;
    }
  }
  out_pos[v] = p;
}
)GLSL";

ShapeKeySkinningManager &ShapeKeySkinningManager::instance()
{
  static ShapeKeySkinningManager mgr;
  return mgr;
}

ShapeKeySkinningManager::ShapeKeySkinningManager() : impl_(new Impl()) {}
ShapeKeySkinningManager::~ShapeKeySkinningManager() {}

void ShapeKeySkinningManager::ensure_static_resources(Object *ob_eval, Mesh *orig_mesh)
{
  if (!orig_mesh) {
    return;
  }
  Impl::MeshStaticData &msd = impl_->static_map.lookup_or_add_default(orig_mesh);
  if (msd.verts_num == orig_mesh->verts_num) {
    return;
  }

  const int verts = orig_mesh->verts_num;
  msd.verts_num = verts;
  msd.rest_positions.clear();
  msd.deltas.clear();
  msd.key_count = 0;

  msd.rest_positions.resize(size_t(verts) * 4);

  blender::Span<blender::float3> pos_span = orig_mesh->vert_positions();
  for (int i = 0; i < verts; ++i) {
    msd.rest_positions[size_t(i) * 4 + 0] = pos_span[i].x;
    msd.rest_positions[size_t(i) * 4 + 1] = pos_span[i].y;
    msd.rest_positions[size_t(i) * 4 + 2] = pos_span[i].z;
    msd.rest_positions[size_t(i) * 4 + 3] = 1.0f;
  }

  /* Build deltas from Key blocks (skip basis / refkey). */
  Key *key = orig_mesh->key;
  if (!key) {
    msd.key_count = 0;
    return;
  }

  /* Count non-ref keyblocks */
  int kcount = 0;
  for (KeyBlock *kb = static_cast<KeyBlock *>(key->block.first); kb; kb = kb->next) {
    if (kb != key->refkey) {
      kcount++;
    }
  }
  msd.key_count = kcount;
  if (kcount == 0) {
    return;
  }

  msd.deltas.resize(size_t(kcount) * size_t(verts) * 4);

  float *refdata = static_cast<float *>(key->refkey ? key->refkey->data : nullptr);

  int kidx = 0;
  for (KeyBlock *kb = static_cast<KeyBlock *>(key->block.first); kb; kb = kb->next) {
    if (kb == key->refkey) {
      continue;
    }
    float *kbdata = static_cast<float *>(kb->data);
    if (!kbdata || !refdata) {
      /* zero delta */
      for (int v = 0; v < verts; ++v) {
        size_t base = (size_t(kidx) * size_t(verts) + size_t(v)) * 4u;
        msd.deltas[base + 0] = 0.0f;
        msd.deltas[base + 1] = 0.0f;
        msd.deltas[base + 2] = 0.0f;
        msd.deltas[base + 3] = 0.0f;
      }
      kidx++;
      continue;
    }
    for (int v = 0; v < verts; ++v) {
      size_t base = (size_t(kidx) * size_t(verts) + size_t(v)) * 4u;
      float rx = refdata[size_t(v) * 3 + 0];
      float ry = refdata[size_t(v) * 3 + 1];
      float rz = refdata[size_t(v) * 3 + 2];
      float kx = kbdata[size_t(v) * 3 + 0];
      float ky = kbdata[size_t(v) * 3 + 1];
      float kz = kbdata[size_t(v) * 3 + 2];
      msd.deltas[base + 0] = kx - rx;
      msd.deltas[base + 1] = ky - ry;
      msd.deltas[base + 2] = kz - rz;
      msd.deltas[base + 3] = 0.0f;
    }
    kidx++;
  }

  /* Mark pending GPU setup so the GL pass will create SSBOs. */
  msd.pending_gpu_setup = true;
  msd.gpu_setup_attempts = 0;
}

/* Dispatch shapekey compute + scatter. Returns true on GPU success. */
bool ShapeKeySkinningManager::dispatch_shapekeys(Depsgraph *depsgraph,
                                                 Object *ob_eval,
                                                 MeshBatchCache *cache,
                                                 blender::gpu::VertBuf *vbo_pos,
                                                 blender::gpu::VertBuf *vbo_nor)
{
  if (!ob_eval || ob_eval->type != OB_MESH) {
    return false;
  }

  Mesh *mesh_eval = static_cast<Mesh *>(ob_eval->data);
  if (!mesh_eval || !mesh_eval->runtime || !mesh_eval->runtime->batch_cache) {
    return false;
  }
  Mesh *mesh_owner = (cache && cache->mesh_owner) ? cache->mesh_owner : nullptr;
  if (!mesh_owner) {
    return false;
  }

  Impl::MeshStaticData *msd_ptr = impl_->static_map.lookup_ptr(mesh_owner);
  if (!msd_ptr) {
    return false;
  }
  Impl::MeshStaticData &msd = *msd_ptr;

  const int verts = msd.verts_num;
  const int kcount = msd.key_count;
  if (verts == 0 || kcount == 0) {
    return false;
  }

  const std::string key_rest = "shapekey_rest_pos";
  const std::string key_deltas = "shapekey_deltas";
  const std::string key_weights = "shapekey_weights";
  const std::string key_out = "shapekey_out_pos";

  /* Ensure internal resources container exists. */
  MeshGpuInternalResources *ires = BKE_mesh_gpu_internal_resources_ensure(mesh_owner);
  if (!ires) {
    return false;
  }

  /* Ensure SSBOs and upload if missing. */
  blender::gpu::StorageBuf *ssbo_rest = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_rest);
  if (!ssbo_rest) {
    ssbo_rest = BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, key_rest, sizeof(float) * size_t(verts) * 4u);
    if (!ssbo_rest)
      return false;
    GPU_storagebuf_update(ssbo_rest, msd.rest_positions.data());
  }

  blender::gpu::StorageBuf *ssbo_deltas = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_deltas);
  if (!ssbo_deltas) {
    ssbo_deltas = BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, key_deltas, sizeof(float) * size_t(kcount) * size_t(verts) * 4u);
    if (!ssbo_deltas)
      return false;
    GPU_storagebuf_update(ssbo_deltas, msd.deltas.data());
  }

  /* Ensure out SSBO */
  blender::gpu::StorageBuf *ssbo_out = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_out);
  if (!ssbo_out) {
    ssbo_out = BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, key_out, sizeof(float) * size_t(verts) * 4u);
    if (!ssbo_out)
      return false;
  }

  /* Prepare weights array (simple curval per KeyBlock; no per-vertex vgroup support here) */
  Key *key = mesh_owner->key;
  if (!key) {
    return false;
  }
  std::vector<float> weights;
  weights.reserve(kcount);
  if (key->type & KEY_RELATIVE) {
    for (KeyBlock *kb = (KeyBlock *)key->block.first; kb; kb = kb->next) {
      if (kb == key->refkey)
        continue;
      weights.push_back(kb->curval);
    }
  }
  else {
    // mode absolu — conversion unités :
    const float t = mesh_owner->key->ctime;  // eval_time en frames
    // construire liste keyframes en frames
    std::vector<float> kpos;
    for (KeyBlock *kb = (KeyBlock *)key->block.first; kb; kb = kb->next) {
      if (kb == key->refkey)
        continue;
      kpos.push_back(kb->pos * 100.0f);  // kb->pos -> frame
    }
    // interpolation LINÉAIRE (simple implémentation)
    weights.assign(kpos.size(), 0.0f);
    if (kpos.size() == 1) {
      weights[0] = 1.0f;
    }
    else {
      // trouver intervalle
      int i = 0;
      while (i + 1 < (int)kpos.size() && t >= kpos[i + 1])
        ++i;
      if (i + 1 >= (int)kpos.size()) {
        weights.back() = 1.0f;
      }
      else {
        float p0 = kpos[i], p1 = kpos[i + 1];
        float u = (p1 == p0) ? 0.0f : (t - p0) / (p1 - p0);
        weights[i] = 1.0f - u;
        weights[i + 1] = u;
      }
    }
  }
  blender::gpu::StorageBuf *ssbo_w = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_weights);
  if (!ssbo_w) {
    ssbo_w = BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, key_weights, sizeof(float) * weights.size());
    if (!ssbo_w)
      return false;
  }
  GPU_storagebuf_update(ssbo_w, weights.data());

  /* Create/ensure compute shader */
  using namespace blender::gpu::shader;
  ShaderCreateInfo info("BGE_ShapeKey_Compute");
  info.local_group_size(256, 1, 1);
  info.compute_source("draw_colormanagement_lib.glsl");
  info.compute_source_generated = shapekey_compute_src;
  info.storage_buf(0, Qualifier::read, "vec4", "rest_pos[]");
  info.storage_buf(1, Qualifier::read, "vec4", "deltas[]");
  info.storage_buf(2, Qualifier::read, "float", "weights[]");
  info.storage_buf(3, Qualifier::write, "vec4", "out_pos[]");
  info.push_constant(Type::int_t, "u_vert_count");
  info.push_constant(Type::int_t, "u_key_count");

  const std::string shader_key = "shapekey_compute";
  blender::gpu::Shader *compute_sh = BKE_mesh_gpu_internal_shader_ensure(
      mesh_owner, shader_key, info);
  if (!compute_sh) {
    return false;
  }

  /* Bind and dispatch compute */
  GPU_shader_bind(compute_sh);
  GPU_storagebuf_bind(ssbo_rest, 0);
  GPU_storagebuf_bind(ssbo_deltas, 1);
  GPU_storagebuf_bind(ssbo_w, 2);
  GPU_storagebuf_bind(ssbo_out, 3);

  int vert_loc = GPU_shader_get_uniform(compute_sh, "u_vert_count");
  if (vert_loc != -1) {
    GPU_shader_uniform_int_ex(compute_sh, vert_loc, 1, 1, &verts);
  }
  int keycount_loc = GPU_shader_get_uniform(compute_sh, "u_key_count");
  if (keycount_loc != -1) {
    GPU_shader_uniform_int_ex(compute_sh, keycount_loc, 1, 1, &kcount);
  }

  const int group_size = 256;
  int groups = (verts + group_size - 1) / group_size;
  GPU_compute_dispatch(compute_sh, groups, 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  /* Scatter to corners (recompute normals) using BKE helper.
   * Provide positions_out SSBO as positions_in for scatter pass; premat/transform injected if
   * missing. */
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
    b.buffer = ssbo_out;
    caller_bindings.push_back(b);
  }
  /* Let BKE_mesh_gpu_run_compute inject transform_mat default if needed. */
  auto post_bind_fn = [](blender::gpu::Shader * /*sh*/) {};
  auto config_fn = [](blender::gpu::shader::ShaderCreateInfo & /* info */) {};

  /* Call scatter: it will handle topology and write into VBOs. */
  blender::bke::GpuComputeStatus status = BKE_mesh_gpu_scatter_to_corners(
      depsgraph,
      ob_eval,
      blender::Span<blender::bke::GpuMeshComputeBinding>(caller_bindings),
      config_fn,
      post_bind_fn,
      mesh_eval->corners_num);

  return status == blender::bke::GpuComputeStatus::Success;
}

void ShapeKeySkinningManager::free_resources_for_mesh(Mesh *mesh)
{
  if (!mesh)
    return;
  if (auto *it = impl_->static_map.lookup_ptr(mesh)) {
    impl_->static_map.remove(mesh);
  }
}

void ShapeKeySkinningManager::free_all()
{
  impl_->static_map.clear();
}
