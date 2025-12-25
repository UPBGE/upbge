/* SPDX-FileCopyrightText:2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ShapeKey GPU blending manager (squelette)
 */

#include "draw_shapekeys_skinning.hh"

#include "BLI_hash.h"
#include "BLI_map.hh"
#include "BLI_threads.h"
#include "BLI_vector.hh"

#include "BKE_key.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_gpu.hh"
#include "BKE_object.hh"
#include "BKE_scene.hh" /* For BKE_scene_ctime_get */

#include "GPU_capabilities.hh"
#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_storage_buffer.hh"

#include "../gpu/intern/gpu_shader_create_info.hh"

#include "DRW_render.hh"
#include "draw_cache_extract.hh"
#include "draw_cache_impl.hh"

#include "draw_modifier_gpu_utils.hh"

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
    /* Cache last uploaded weights to avoid redundant GPU updates. */
    std::vector<float> prev_weights;
    bool prev_weights_valid = false;
    /* Cache last computed hash to detect ShapeKey changes */
    uint32_t last_verified_hash = 0;
  };

  Map<Mesh *, MeshStaticData> static_map;
};

static const char *shapekey_compute_src = R"GLSL(
/* compute shader: out_pos[v] = rest_pos[v] + sum_k weights[k] * deltas[k*V + v] */

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

/* Compute a hash of the ShapeKey state to detect changes */
uint32_t ShapeKeySkinningManager::compute_shapekey_hash(const Mesh *mesh)
{
  if (!mesh || !mesh->key) {
    return 0;
  }

  uint32_t hash = 0;
  const Key *key = mesh->key;

  /* Hash number of vertices */
  hash = BLI_hash_int_2d(hash, mesh->verts_num);

  /* Hash number of keyblocks */
  int kb_count = 0;
  for (const KeyBlock *kb = static_cast<const KeyBlock *>(key->block.first); kb; kb = kb->next) {
    kb_count++;
  }
  hash = BLI_hash_int_2d(hash, kb_count);

  /* Hash refkey pointer (detects Basis change) */
  hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(key->refkey)));

  /* Hash each KeyBlock state */
  for (const KeyBlock *kb = static_cast<const KeyBlock *>(key->block.first); kb; kb = kb->next) {
    /* Hash relative target (detects "Relative To" changes) */
    hash = BLI_hash_int_2d(hash, uint32_t(kb->relative));

    /* Hash totelem (detects geometry changes) */
    hash = BLI_hash_int_2d(hash, uint32_t(kb->totelem));

    /* Hash data pointer (detects Edit Mode changes in ShapeKey geometry) */
    hash = BLI_hash_int_2d(hash, uint32_t(reinterpret_cast<uintptr_t>(kb->data)));
  }

  return hash;
}

void ShapeKeySkinningManager::ensure_static_resources(Mesh *orig_mesh, uint32_t pipeline_hash)
{
  if (!orig_mesh) {
    return;
  }

  Impl::MeshStaticData &msd = impl_->static_map.lookup_or_add_default(orig_mesh);

  /* Check if recalculation is needed by comparing pipeline hash.
   * The hash is computed by
   * GPUModifierPipeline and includes ALL ShapeKey state
   * (vertex count, Basis, Relative To,
   * Edit Mode changes, etc.)
   * 
   * We recalculate CPU deltas when:
   * 1. First time
   * (last_verified_hash == 0)
   * 2. Hash changed (pipeline_hash != last_verified_hash)
   * 3.
   * GPU resources were invalidated (pending_gpu_setup == true) */
  const bool first_time = (msd.last_verified_hash == 0);
  const bool hash_changed = (pipeline_hash != msd.last_verified_hash);
  const bool gpu_invalidated = msd.pending_gpu_setup;

  if (!first_time && !hash_changed && !gpu_invalidated) {
    return;  // No changes detected, reuse cached deltas
  }

  /* Recalculate deltas (triggered by hash change or GPU invalidation) */
  if (0) {
    printf("Recalculating ShapeKey deltas for mesh '%s' (first=%d, hash_changed=%d, gpu_inv=%d)\n",
           (orig_mesh->id.name + 2),
           first_time,
           hash_changed,
           gpu_invalidated);
  }

  /* Update hash cache */
  msd.last_verified_hash = pipeline_hash;

  const int verts = orig_mesh->verts_num;
  msd.verts_num = verts;
  msd.rest_positions.clear();
  msd.deltas.clear();
  msd.key_count = 0;

  msd.rest_positions.resize(size_t(verts) * 4);

  /* Get base positions: if we have an active shape key, use it as the base,
   * otherwise use the mesh's rest positions */
  Key *key = orig_mesh->key;
  KeyBlock *base_kb = key ? key->refkey : nullptr;  // Default to Basis

  /* Note: We can't access ob->shapenr here (no Object available in ensure_static_resources),
   * but that's OK: the active key only matters for Edit Mode (BMesh), which doesn't apply
   * to GPU deformation. For GPU, we always use Basis as the base for deltas. */

  if (base_kb && base_kb->data && base_kb->totelem == verts) {
    /* Use Basis shape key data as rest positions */
    const float *basis_data = static_cast<const float *>(base_kb->data);
    for (int i = 0; i < verts; ++i) {
      msd.rest_positions[size_t(i) * 4 + 0] = basis_data[i * 3 + 0];
      msd.rest_positions[size_t(i) * 4 + 1] = basis_data[i * 3 + 1];
      msd.rest_positions[size_t(i) * 4 + 2] = basis_data[i * 3 + 2];
      msd.rest_positions[size_t(i) * 4 + 3] = 1.0f;
    }
  }
  else {
    /* Fallback: use mesh rest positions */
    blender::Span<blender::float3> pos_span = orig_mesh->vert_positions();
    for (int i = 0; i < verts; ++i) {
      msd.rest_positions[size_t(i) * 4 + 0] = pos_span[i].x;
      msd.rest_positions[size_t(i) * 4 + 1] = pos_span[i].y;
      msd.rest_positions[size_t(i) * 4 + 2] = pos_span[i].z;
      msd.rest_positions[size_t(i) * 4 + 3] = 1.0f;
    }
  }

  /* Build deltas from Key blocks (skip basis / refkey). */
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

  int kidx = 0;
  for (KeyBlock *kb = static_cast<KeyBlock *>(key->block.first); kb; kb = kb->next) {
    if (kb == key->refkey) {
      continue;
    }
    float *kbdata = static_cast<float *>(kb->data);

    /* Find the correct reference key (Basis or another key via kb->relative) */
    KeyBlock *ref_kb = nullptr;
    if (kb->relative == 0) {
      /* Relative to Basis (refkey) */
      ref_kb = key->refkey;
    }
    else {
      /* Relative to another key at index kb->relative */
      int ref_idx = 0;
      for (KeyBlock *kb_search = static_cast<KeyBlock *>(key->block.first); kb_search;
           kb_search = kb_search->next, ref_idx++)
      {
        if (ref_idx == kb->relative) {
          ref_kb = kb_search;
          break;
        }
      }
      /* Fallback to refkey if not found */
      if (!ref_kb) {
        ref_kb = key->refkey;
      }
    }
    float *ref_data = ref_kb ? static_cast<float *>(ref_kb->data) : nullptr;
    if (!kbdata || !ref_data) {
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
    /* Compute delta from the correct reference key */
    for (int v = 0; v < verts; ++v) {
      size_t base = (size_t(kidx) * size_t(verts) + size_t(v)) * 4u;
      float rx = ref_data[size_t(v) * 3 + 0];
      float ry = ref_data[size_t(v) * 3 + 1];
      float rz = ref_data[size_t(v) * 3 + 2];
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

  if (first_time || hash_changed) {
    msd.pending_gpu_setup = true;
    msd.gpu_setup_attempts = 0;
  }
}

/* Dispatch shapekey compute + scatter. Returns true on GPU success. */
blender::gpu::StorageBuf *ShapeKeySkinningManager::dispatch_shapekeys(
    MeshBatchCache *cache, Object *deformed_eval)
{
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
  if (!draw_modifier_gpu_setup_retry(msd.pending_gpu_setup, msd.gpu_setup_attempts)) {
    return nullptr;
  }

  blender::bke::MeshGpuData *mesh_gpu_data = BKE_mesh_gpu_ensure_data(mesh_owner,
                                                                      (Mesh *)deformed_eval->data);
  if (!mesh_gpu_data) {
    return nullptr;
  }

  /* GPU resources ensured successfully: clear pending flag so subsequent calls proceed. */
  if (msd.pending_gpu_setup) {
    msd.pending_gpu_setup = false;
    msd.gpu_setup_attempts = 0;
  }

  const int verts = msd.verts_num;
  const int kcount = msd.key_count;
  if (verts == 0 || kcount == 0) {
    return nullptr;
  }

  const std::string key_rest = "shapekey_rest_pos";
  const std::string key_deltas = "shapekey_deltas";
  const std::string key_weights = "shapekey_weights";
  const std::string key_out = "shapekey_out_pos";

  /* Ensure SSBOs and upload if missing. */
  blender::gpu::StorageBuf *ssbo_rest = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_rest);
  if (!ssbo_rest) {
    ssbo_rest = BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, key_rest, sizeof(float) * size_t(verts) * 4u);
    if (!ssbo_rest) {
      return nullptr;
    }
    GPU_storagebuf_update(ssbo_rest, msd.rest_positions.data());
  }

  blender::gpu::StorageBuf *ssbo_deltas = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_deltas);
  if (!ssbo_deltas) {
    ssbo_deltas = BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, key_deltas, sizeof(float) * size_t(kcount) * size_t(verts) * 4u);
    if (!ssbo_deltas) {
      return nullptr;
    }
    GPU_storagebuf_update(ssbo_deltas, msd.deltas.data());
  }

  /* Ensure out SSBO */
  blender::gpu::StorageBuf *ssbo_out_local = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_out);
  if (!ssbo_out_local) {
    ssbo_out_local = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_out);
    if (!ssbo_out_local) {
      ssbo_out_local = BKE_mesh_gpu_internal_ssbo_ensure(
          mesh_owner, key_out, sizeof(float) * size_t(verts) * 4u);
      if (!ssbo_out_local) {
        return nullptr;
      }
    }
  }

  /* Prepare weights array (simple curval per KeyBlock; no per-vertex vgroup support here) */
  Key *key = mesh_owner->key;
  if (!key) {
    return nullptr;
  }

  std::vector<float> weights;
  weights.reserve(kcount);
  if (key->type & KEY_RELATIVE) {
    /* Find active shape key (the base for relative shapes) from Object->shapenr */
    KeyBlock *active_kb = nullptr;

    /* Use ob_eval to access shapenr (active shape index) */
    if (deformed_eval && deformed_eval->data == mesh_owner && deformed_eval->shapenr > 0) {
      int active_index = deformed_eval->shapenr - 1;  // shapenr is 1-indexed (0 = no shape)
      int idx = 0;
      for (KeyBlock *kb = (KeyBlock *)key->block.first; kb; kb = kb->next, idx++) {
        if (idx == active_index) {
          active_kb = kb;
          break;
        }
      }
    }
    for (KeyBlock *kb = (KeyBlock *)key->block.first; kb; kb = kb->next) {
      if (kb == key->refkey)
        continue;

      /* Skip active shape key (it's the base, not a deformation) */
      if (active_kb && kb == active_kb) {
        weights.push_back(0.0f);
        continue;
      }

      /* Skip muted keys */
      if (kb->flag & KEYBLOCK_MUTE) {
        weights.push_back(0.0f);
        continue;
      }

      /* Clamp curval with slidermin/max */
      float w = kb->curval;
      w = std::clamp(w, kb->slidermin, kb->slidermax);
      weights.push_back(w);
    }
  }
  else {
    // mode absolute
    const float t = key->ctime / 100.0f;

    // build keyframes list in frames
    std::vector<float> kpos;
    std::vector<KeyBlock *> kblocks;
    for (KeyBlock *kb = (KeyBlock *)key->block.first; kb; kb = kb->next) {
      if (kb == key->refkey)
        continue;
      kpos.push_back(kb->pos * 100.0f);  // kb->pos -> frame
      kblocks.push_back(kb);
    }
    // Linear interpolation (simple implementation)
    weights.assign(kpos.size(), 0.0f);
    if (kpos.size() == 1) {
      weights[0] = (kblocks[0]->flag & KEYBLOCK_MUTE) ? 0.0f : 1.0f;
    }
    else {
      // find interval
      int i = 0;
      while (i + 1 < (int)kpos.size() && t >= kpos[i + 1])
        ++i;
      if (i + 1 >= (int)kpos.size()) {
        weights.back() = (kblocks.back()->flag & KEYBLOCK_MUTE) ? 0.0f : 1.0f;
      }
      else {
        float p0 = kpos[i], p1 = kpos[i + 1];
        float u = (p1 == p0) ? 0.0f : (t - p0) / (p1 - p0);
        weights[i] = (kblocks[i]->flag & KEYBLOCK_MUTE) ? 0.0f : (1.0f - u);
        weights[i + 1] = (kblocks[i + 1]->flag & KEYBLOCK_MUTE) ? 0.0f : u;
      }
    }
  }
  blender::gpu::StorageBuf *ssbo_w = BKE_mesh_gpu_internal_ssbo_get(mesh_owner, key_weights);
  if (!ssbo_w) {
    ssbo_w = BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_owner, key_weights, sizeof(float) * weights.size());
    if (!ssbo_w) {
      return nullptr;
    }
    /* Freshly created buffer: upload weights. */
    GPU_storagebuf_update(ssbo_w, weights.data());
    msd.prev_weights = weights;
    msd.prev_weights_valid = true;
  }
  else {
    /* Compare to previous weights with small epsilon to avoid noise updates. */
    bool changed = !msd.prev_weights_valid || (msd.prev_weights.size() != weights.size());
    const float EPS = 1e-6f;
    if (!changed) {
      for (size_t i = 0; i < weights.size(); ++i) {
        if (fabsf(weights[i] - msd.prev_weights[i]) > EPS) {
          changed = true;
          break;
        }
      }
    }
    if (changed) {
      GPU_storagebuf_update(ssbo_w, weights.data());
      msd.prev_weights = weights;
      msd.prev_weights_valid = true;
    }
    /* If not changed, skip upload (compute still runs, but upload cost avoided). */
  }

  /* Create/ensure compute shader */
  using namespace blender::gpu::shader;
  ShaderCreateInfo info("pyGPU_Shader");
  info.local_group_size(256, 1, 1);
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
    return nullptr;
  }

  /* Bind and dispatch compute */
  const blender::gpu::shader::SpecializationConstants *constants =
      &GPU_shader_get_default_constant_state(compute_sh);
  GPU_shader_bind(compute_sh, constants);
  GPU_storagebuf_bind(ssbo_rest, 0);
  GPU_storagebuf_bind(ssbo_deltas, 1);
  GPU_storagebuf_bind(ssbo_w, 2);
  GPU_storagebuf_bind(ssbo_out_local, 3);

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
  GPU_compute_dispatch(compute_sh, groups, 1, 1, constants);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  /* Return SSBO containing computed positions. Caller will scatter_to_corners when needed. */
  return ssbo_out_local;
}

void ShapeKeySkinningManager::free_resources_for_mesh(Mesh *mesh)
{
  if (!mesh)
    return;
  if (auto *it = impl_->static_map.lookup_ptr(mesh)) {
    impl_->static_map.remove(mesh);
  }
}

void ShapeKeySkinningManager::invalidate_all(Mesh *mesh)
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
    /* Keep CPU data (deltas, rest_positions, etc.) for fast recreation */
  }
}

void ShapeKeySkinningManager::free_all()
{
  impl_->static_map.clear();
}
