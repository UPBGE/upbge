/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "mesh_gpu_cache.hh"

#include "BKE_mesh_gpu.hh"

#include <fmt/format.h>
#include <mutex>
#include <unordered_map>

#include "BKE_mesh.hh"
#include "BKE_scene.hh"

#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_task.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "GPU_capabilities.hh"
#include "GPU_compute.hh"
#include "GPU_context.hh"
#include "GPU_state.hh"

#include "DEG_depsgraph_query.hh"

#include "WM_api.hh"

#include "../draw/intern/draw_cache_extract.hh"

using blender::bke::MeshGPUCacheManager;
using blender::bke::MeshGpuData;

/* Implementation of the orphans flush previously local to this file. This is the
 * actual function that performs GPU frees. The public wrapper in mesh_gpu_cache.cc
 * calls this implementation. */
void mesh_gpu_orphans_flush_impl()
{
  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());

  if (!GPU_context_active_get()) {
    return;
  }

  auto &orphan_list = MeshGPUCacheManager::get().orphans();
  for (MeshGpuData &d : orphan_list) {
    for (auto &pair : d.compute_shaders) {
      if (pair.second) {
        GPU_shader_free(pair.second);
        pair.second = nullptr;
      }
    }
    d.compute_shaders.clear();
    if (d.internal_resources) {
      for (auto *ssbo : d.internal_resources->ssbos) {
        if (ssbo) {
          GPU_storagebuf_free(ssbo);
        }
      }
      for (auto *sh : d.internal_resources->shaders) {
        if (sh) {
          GPU_shader_free(sh);
        }
      }
      delete d.internal_resources;
      d.internal_resources = nullptr;
    }
    BKE_mesh_gpu_topology_free(d.topology);
  }
  orphan_list.clear();
}

/* Note: functions now use MeshGPUCacheManager accessors instead of globals. */

bool BKE_mesh_gpu_topology_create(const Mesh *mesh, blender::bke::MeshGPUTopology &topology)
{
  if (!mesh) {
    return false;
  }

  /* Clear any existing data */
  BKE_mesh_gpu_topology_free(topology);

  /* Get mesh topology data */
  const auto face_offsets = mesh->face_offsets();
  const auto corner_to_face = mesh->corner_to_face_map();
  const auto corner_verts_span = mesh->corner_verts();
  const auto corner_tris = mesh->corner_tris();
  const auto corner_tri_faces = mesh->corner_tri_faces();
  const auto edges = mesh->edges();
  const auto corner_edges_span = mesh->corner_edges();

  /* Convert spans to vectors for easier handling */
  blender::Vector<int> corner_verts_vec(corner_verts_span.begin(), corner_verts_span.end());
  blender::Vector<int> corner_tris_flat;
  corner_tris_flat.reserve(corner_tris.size() * 3);
  for (const blender::int3 &tri : corner_tris) {
    corner_tris_flat.append(tri.x);
    corner_tris_flat.append(tri.y);
    corner_tris_flat.append(tri.z);
  }
  blender::Vector<int> corner_tri_faces_vec(corner_tri_faces.begin(), corner_tri_faces.end());

  blender::Vector<int> edges_flat;
  edges_flat.reserve(edges.size() * 2);
  for (const blender::int2 &edge : edges) {
    edges_flat.append(edge.x);
    edges_flat.append(edge.y);
  }

  blender::Vector<int> corner_edges_vec(corner_edges_span.begin(), corner_edges_span.end());

  /* Get vertex-to-face mapping */
  const blender::OffsetIndices<int> v2f_off = mesh->vert_to_face_map_offsets();
  const blender::GroupedSpan<int> v2f = mesh->vert_to_face_map();

  const int v2f_offsets_size = v2f_off.size();
  blender::Vector<int> v2f_offsets(v2f_offsets_size);
  for (int v = 0; v < v2f_offsets_size; ++v) {
    v2f_offsets[v] = v2f_off.data()[v];
  }
  const int total_v2f = v2f_offsets.is_empty() ? 0 : v2f_offsets.last();

  blender::Vector<int> v2f_indices;
  v2f_indices.resize(std::max(total_v2f, 0));
  if (v2f_offsets_size > 0) {
    blender::threading::parallel_for(
        blender::IndexRange(v2f_offsets_size - 1), 4096, [&](const blender::IndexRange range) {
          for (int v : range) {
            const blender::Span<int> faces_v = v2f[v];
            const int dst = v2f_off.data()[v];
            if (!faces_v.is_empty()) {
              std::copy(faces_v.begin(), faces_v.end(), v2f_indices.begin() + dst);
            }
          }
        });
  }

  /* Compute offsets for packed buffer */
  topology.face_offsets_offset = 0;
  topology.corner_to_face_offset = topology.face_offsets_offset + int(face_offsets.size());
  topology.corner_verts_offset = topology.corner_to_face_offset + int(corner_to_face.size());
  topology.corner_tris_offset = topology.corner_verts_offset + int(corner_verts_vec.size());
  topology.corner_tri_faces_offset = topology.corner_tris_offset + int(corner_tris_flat.size());
  topology.edges_offset = topology.corner_tri_faces_offset + int(corner_tri_faces_vec.size());
  topology.corner_edges_offset = topology.edges_offset + int(edges_flat.size());
  topology.vert_to_face_offsets_offset = topology.corner_edges_offset +
                                         int(corner_edges_vec.size());
  topology.vert_to_face_offset = topology.vert_to_face_offsets_offset + int(v2f_offsets.size());
  topology.total_size = topology.vert_to_face_offset + int(v2f_indices.size());

  /* Pack into single int vector */
  topology.data.clear();
  topology.data.reserve(topology.total_size);
  topology.data.extend(face_offsets);
  topology.data.extend(corner_to_face);
  topology.data.extend(corner_verts_vec);
  topology.data.extend(corner_tris_flat);
  topology.data.extend(corner_tri_faces_vec);
  topology.data.extend(edges_flat);
  topology.data.extend(corner_edges_vec);
  topology.data.extend(v2f_offsets);
  topology.data.extend(v2f_indices);

  return true;
}

bool BKE_mesh_gpu_topology_upload(blender::bke::MeshGPUTopology &topology)
{
  if (topology.data.is_empty()) {
    return false;
  }

  if (!GPU_context_active_get()) {
    return false;
  }

  /* Free existing SSBO if present */
  if (topology.ssbo) {
    GPU_storagebuf_free(topology.ssbo);
    topology.ssbo = nullptr;
  }

  /* Create and upload new SSBO */
  topology.ssbo = GPU_storagebuf_create(sizeof(int) * topology.total_size);
  if (!topology.ssbo) {
    return false;
  }

  GPU_storagebuf_update(topology.ssbo, topology.data.data());
  return true;
}

void BKE_mesh_gpu_topology_free(blender::bke::MeshGPUTopology &topology)
{
  if (topology.ssbo) {
    if (GPU_context_active_get()) {
      GPU_storagebuf_free(topology.ssbo);
    }
    /* If no GPU context, the SSBO will be cleaned up by GPU module cleanup */
    topology.ssbo = nullptr;
  }
  topology.data.clear();
  topology.total_size = 0;
}

static const char *scatter_to_corners_main_glsl = R"GLSL(
// 10_10_10_2 packing utility
int pack_i10_trunc(float x) {
  const int signed_int_10_max = 511;
  const int signed_int_10_min = -512;
  float s = x * float(signed_int_10_max);
  int q = int(s);
  q = clamp(q, signed_int_10_min, signed_int_10_max);
  return q & 0x3FF;
}

uint pack_norm(vec3 n) {
  int nx = pack_i10_trunc(n.x);
  int ny = pack_i10_trunc(n.y);
  int nz = pack_i10_trunc(n.z);
  return uint(nx) | (uint(ny) << 10) | (uint(nz) << 20);
}

int pack_i16_trunc(float x) {
  return clamp(int(round(x * 32767.0)), -32768, 32767);
}
uint pack_i16_pair(float a, float b) {
  return (uint(pack_i16_trunc(a)) & 0xFFFFu) | ((uint(pack_i16_trunc(b)) & 0xFFFFu) << 16);
}

vec3 newell_face_normal_object(int f) {
  int beg = face_offsets(f);
  int end = face_offsets(f + 1);
  vec3 n = vec3(0.0);
  int v_prev_idx = corner_verts(end - 1);
  vec3 v_prev = positions_in[v_prev_idx].xyz;
  for (int i = beg; i < end; ++i) {
    int v_curr_idx = corner_verts(i);
    vec3 v_curr = positions_in[v_curr_idx].xyz;
    n += cross(v_prev, v_curr);
    v_prev = v_curr;
  }
  return normalize(n);
}

vec3 transform_normal(vec3 n, mat4 m) {
  return transpose(inverse(mat3(m))) * n;
}

void main() {
  uint c = gl_GlobalInvocationID.x;
  if (c >= positions_out.length()) {
    return;
  }

  int v = corner_verts(int(c));

  // 1) Scatter position
  vec4 p_obj = positions_in[v];
  positions_out[c] = transform_mat[0] * p_obj;

  // 2) Calculate and scatter normal
  vec3 n_obj;
  if (normals_domain == 1) { // Face
    int f = corner_to_face(int(c));
    n_obj = newell_face_normal_object(f);
  }
  else { // Point
    int beg = vert_to_face_offsets(v);
    int end = vert_to_face_offsets(v + 1);
    vec3 n_accum = vec3(0.0);
    for (int i = beg; i < end; ++i) {
      int f = vert_to_face(i);
      n_accum += newell_face_normal_object(f);
    }
    n_obj = n_accum;
  }

  vec3 n_world = transform_normal(n_obj, transform_mat[0]);
  n_world = normalize(n_world);

  if (normals_hq == 0) {
    normals_out[c] = pack_norm(n_world);
  }
  else {
    int base = int(c) * 2;
    normals_out[base + 0] = pack_i16_pair(n_world.x, n_world.y);
    normals_out[base + 1] = pack_i16_pair(n_world.z, 0.0);
  }
}
)GLSL";

std::string BKE_mesh_gpu_topology_glsl_accessors_string(
    const blender::bke::MeshGPUTopology &topology)
{
  return fmt::format(R"GLSL(
// Mesh topology accessors (generated)
int face_offsets(int i) {{ return topo[{} + i]; }}
int corner_to_face(int i) {{ return topo[{} + i]; }}
int corner_verts(int i) {{ return topo[{} + i]; }}
int corner_tri(int tri_idx, int vert_idx) {{ return topo[{} + tri_idx * 3 + vert_idx]; }}
int corner_tri_face(int i) {{ return topo[{} + i]; }}
int2 edges(int i) {{ return int2(topo[{} + i * 2], topo[{} + i * 2 + 1]); }}
int corner_edges(int i) {{ return topo[{} + i]; }}
int vert_to_face_offsets(int i) {{ return topo[{} + i]; }}
int vert_to_face(int i) {{ return topo[{} + i]; }}
)GLSL",
                     topology.face_offsets_offset,
                     topology.corner_to_face_offset,
                     topology.corner_verts_offset,
                     topology.corner_tris_offset,
                     topology.corner_tri_faces_offset,
                     topology.edges_offset,
                     topology.edges_offset,
                     topology.corner_edges_offset,
                     topology.vert_to_face_offsets_offset,
                     topology.vert_to_face_offset);
}

void BKE_mesh_gpu_topology_add_specialization_constants(
    blender::gpu::shader::ShaderCreateInfo &info, const blender::bke::MeshGPUTopology &topology)
{
  using namespace blender::gpu::shader;
  info.specialization_constant(Type::int_t, "face_offsets_offset", topology.face_offsets_offset);
  info.specialization_constant(
      Type::int_t, "corner_to_face_offset", topology.corner_to_face_offset);
  info.specialization_constant(Type::int_t, "corner_verts_offset", topology.corner_verts_offset);
  info.specialization_constant(Type::int_t, "corner_tris_offset", topology.corner_tris_offset);
  info.specialization_constant(
      Type::int_t, "corner_tri_faces_offset", topology.corner_tri_faces_offset);
  info.specialization_constant(Type::int_t, "edges_offset", topology.edges_offset);
  info.specialization_constant(Type::int_t, "corner_edges_offset", topology.corner_edges_offset);
  info.specialization_constant(
      Type::int_t, "vert_to_face_offsets_offset", topology.vert_to_face_offsets_offset);
  info.specialization_constant(Type::int_t, "vert_to_face_offset", topology.vert_to_face_offset);
}

#define MESH_GPU_TOPOLOGY_BINDING 15

/* Helper to check if a bind_name is present (accepts both "name" and "name[]"). */
static bool has_bind_name(const char *name, const std::vector<blender::bke::GpuMeshComputeBinding> &local_bindings)
{
  if (!name) {
    return false;
  }
  for (const auto &b : local_bindings) {
    if (b.bind_name == nullptr) {
      continue;
    }
    if (STR_ELEM(b.bind_name, name)) {
      return true;
    }
    /* accept array form too */
    std::string s = std::string(name) + "[]";
    if (b.bind_name && s == b.bind_name) {
      return true;
    }
  }
  return false;
}

/* Find next free binding index (avoid MESH_GPU_TOPOLOGY_BINDING). */
static int find_free_binding(
    const std::vector<blender::bke::GpuMeshComputeBinding> &local_bindings, int start = 0)
{
  int candidate = start;
  for (;;) {
    bool used = false;
    for (const auto &b : local_bindings) {
      if (b.binding == candidate) {
        used = true;
        break;
      }
    }
    if (candidate == MESH_GPU_TOPOLOGY_BINDING) {
      used = true;
    }
    if (!used) {
      return candidate;
    }
    ++candidate;
  }
}

blender::bke::GpuComputeStatus BKE_mesh_gpu_run_compute(
    const Depsgraph *depsgraph,
    const Object *ob_eval,
    const char *main_glsl,
    const std::vector<blender::bke::GpuMeshComputeBinding> &caller_bindings,
    const std::function<void(blender::gpu::shader::ShaderCreateInfo &)> &config_fn,
    const std::function<void(blender::gpu::Shader *)> &post_bind_fn,
    int dispatch_count)
{
  if (!GPU_context_active_get() || !depsgraph || !ob_eval || ob_eval->type != OB_MESH) {
    return blender::bke::GpuComputeStatus::Error;
  }

  /* Attempt to free any deferred resources now that we are on a GPU context. */
  if (GPU_context_active_get()) {
    MeshGPUCacheManager::get().flush_orphans();
  }

  Object *ob_orig = DEG_get_original(const_cast<Object *>(ob_eval));
  Mesh *mesh_orig = static_cast<Mesh *>(ob_orig->data);
  Mesh *mesh_eval = static_cast<Mesh *>(ob_eval->data);
  if (!mesh_eval) {
    BKE_mesh_gpu_free_for_mesh(mesh_orig);
    return blender::bke::GpuComputeStatus::Error;
  }

  if (!ob_orig) {
    BKE_mesh_gpu_free_for_mesh(mesh_orig);
    return blender::bke::GpuComputeStatus::Error;
  }

  if (ob_orig->mode != OB_MODE_OBJECT) {
    // early return when not in object mode
    BKE_mesh_gpu_free_for_mesh(mesh_orig);
    return blender::bke::GpuComputeStatus::NotReady;
  }

  if (!mesh_eval->runtime || !mesh_eval->runtime->batch_cache) {
    // early return
    BKE_mesh_gpu_free_for_mesh(mesh_orig);
    return blender::bke::GpuComputeStatus::NotReady;
  }

  using namespace blender::draw;

  auto *cache = static_cast<blender::draw::MeshBatchCache *>(mesh_eval->runtime->batch_cache);
  auto *vbo_pos_ptr = cache->final.buff.vbos.lookup_ptr(blender::draw::VBOType::Position);
  if (!vbo_pos_ptr) {
    // early return
    BKE_mesh_gpu_free_for_mesh(mesh_orig);
    return blender::bke::GpuComputeStatus::NotReady;
  }
  auto *vbo_pos = vbo_pos_ptr->get();
  const GPUVertFormat *format = GPU_vertbuf_get_format(vbo_pos);

  if (format->stride == 16 && (ob_orig->id.recalc & ID_RECALC_GEOMETRY) != 0) {
    BKE_mesh_gpu_free_for_mesh(mesh_orig);
    return blender::bke::GpuComputeStatus::NotReady;
  }

  if (format->stride != 16) {
    BKE_mesh_gpu_free_for_mesh(mesh_orig);
    mesh_orig->is_running_gpu_animation_playback = 1;
    mesh_eval->is_running_gpu_animation_playback = 1;
    DEG_id_tag_update(const_cast<ID *>(&DEG_get_original(ob_eval)->id), ID_RECALC_GEOMETRY);
    WM_main_add_notifier(NC_WINDOW, nullptr);
    return blender::bke::GpuComputeStatus::NotReady;
  }

  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());

  auto &mesh_data = MeshGPUCacheManager::get().mesh_cache()[mesh_orig];
  /* Create/upload topology (from evaluated mesh) if needed. On failure cleanup. */
  if (!mesh_data.topology.ssbo) {
    if (!BKE_mesh_gpu_topology_create(mesh_eval, mesh_data.topology) ||
        !BKE_mesh_gpu_topology_upload(mesh_data.topology))
    {
      BKE_mesh_gpu_free_for_mesh(mesh_orig);
      return blender::bke::GpuComputeStatus::Error;
    }
  }

  /* --- Prepare bindings vector, inject defaults for scatter shader if needed --- */
  std::vector<blender::bke::GpuMeshComputeBinding> local_bindings;
  local_bindings.reserve(caller_bindings.size() + 4);
  for (const auto &b : caller_bindings) {
    local_bindings.push_back(b);
  }

  /* Only special-case the scatter shader. If called via scatter_to_corners, ensure we have
   * `positions_in` and `transform_mat` SSBOs available and declared. */
  if (main_glsl && (main_glsl == scatter_to_corners_main_glsl)) {
    const bool has_positions_in = has_bind_name("positions_in", local_bindings);
    const bool has_transform_mat = has_bind_name("transform_mat", local_bindings);

    /* Create default positions_in SSBO from mesh_eval->vert_positions() if missing.
     * Fast-path: check under mutex to avoid expensive work when SSBO already exists.
     * If missing, build CPU buffer outside the mutex, then call ensure (which will
     * safely create-or-return the SSBO). */
    if (!has_positions_in) {
      const std::string key = "scatter_positions_in";
      blender::gpu::StorageBuf *ssbo = nullptr;

      /* Quick check while holding the mutex. */
      if (mesh_data.internal_resources) {
        auto it_ssbo = mesh_data.internal_resources->ssbo_map.find(key);
        if (it_ssbo != mesh_data.internal_resources->ssbo_map.end()) {
          ssbo = it_ssbo->second.first;
        }
      }

      if (!ssbo) {
        const int verts = mesh_eval->verts_num;
        if (verts > 0 && GPU_context_active_get()) {
          const size_t size_bytes = size_t(verts) * sizeof(float) * 4; /* vec4 per vertex */

          /* Build CPU buffer OUTSIDE the mutex to avoid holding the lock during heavy work. */
          lock.unlock();
          blender::Vector<blender::float4> pos_data;
          pos_data.resize(size_t(verts));
          blender::Span<blender::float3> pos_span = mesh_eval->vert_positions();
          blender::threading::parallel_for(
              blender::IndexRange(verts), 4096, [&](blender::IndexRange range) {
                for (int i : range) {
                  pos_data[i] = blender::float4(pos_span[i].xyz(), 1.0f);
                }
              });

          try {
            /* Ensure (may return existing SSBO if another thread created it meanwhile). */
            ssbo = BKE_mesh_gpu_internal_ssbo_ensure(mesh_eval, key, size_bytes);
            if (ssbo) {
              GPU_storagebuf_update(ssbo, pos_data.data());
            }
          }
          catch (...) {
            /* Re-acquire mutex before propagating the exception to keep invariants. */
            lock.lock();
            throw;
          }
          /* Re-acquire mutex for map/binding manipulation. */
          lock.lock();
        }
      }

      /* If we now have an SSBO (existing or newly created), inject binding. */
      if (ssbo) {
        blender::bke::GpuMeshComputeBinding gb;
        gb.binding = find_free_binding(local_bindings, 0);
        gb.buffer = ssbo;
        gb.qualifiers = blender::gpu::shader::Qualifier::read;
        gb.type_name = "vec4";
        gb.bind_name = "positions_in[]";
        local_bindings.push_back(gb);
      }
    }

    /* Create default transform_mat SSBO with identity matrix if missing. */
    if (!has_transform_mat) {
      if (GPU_context_active_get()) {
        const std::string key = "scatter_transform_mat";
        float mat[4][4];
        unit_m4(mat);
        lock.unlock();
        blender::gpu::StorageBuf *ssbo = nullptr;
        try {
          ssbo = BKE_mesh_gpu_internal_ssbo_ensure(mesh_eval, key, sizeof(float) * 16);
          if (ssbo) {
            GPU_storagebuf_update(ssbo, &mat[0][0]);
            blender::bke::GpuMeshComputeBinding gb;
            gb.binding = find_free_binding(local_bindings, 0);
            gb.buffer = ssbo;
            gb.qualifiers = blender::gpu::shader::Qualifier::read;
            gb.type_name = "mat4";
            gb.bind_name = "transform_mat[]";
            local_bindings.push_back(gb);
          }
        }
        catch (...) {
          lock.lock();
          throw;
        }
        lock.lock();
      }
    }

    /* Inject default outputs if caller forgot them: positions_out -> position VBO,
     * normals_out -> corner normal VBO. This mirrors how we inject inputs above. */
    const bool has_positions_out = has_bind_name("positions_out", local_bindings);
    const bool has_normals_out = has_bind_name("normals_out", local_bindings);
    if (!has_positions_out) {
      if (cache) {
        if (auto *pos_ptr = cache->final.buff.vbos.lookup_ptr(blender::draw::VBOType::Position)) {
          blender::gpu::VertBuf *vbo = pos_ptr->get();
          if (vbo) {
            blender::bke::GpuMeshComputeBinding gb = {};
            gb.binding = find_free_binding(local_bindings, 0);
            gb.buffer = vbo;
            gb.qualifiers = blender::gpu::shader::Qualifier::read_write;
            gb.type_name = "vec4";
            gb.bind_name = "positions_out[]";
            local_bindings.push_back(gb);
          }
        }
      }
    }
    if (!has_normals_out) {
      if (cache) {
        if (auto *nor_ptr = cache->final.buff.vbos.lookup_ptr(blender::draw::VBOType::CornerNormal)) {
          blender::gpu::VertBuf *vbo_nor = nor_ptr->get();
          if (vbo_nor) {
            blender::bke::GpuMeshComputeBinding gb = {};
            gb.binding = find_free_binding(local_bindings, 0);
            gb.buffer = vbo_nor;
            gb.qualifiers = blender::gpu::shader::Qualifier::write;
            gb.type_name = "uint";
            gb.bind_name = "normals_out[]";
            local_bindings.push_back(gb);
          }
        }
      }
    }
  }

  std::string glsl_accessors = BKE_mesh_gpu_topology_glsl_accessors_string(mesh_data.topology);
  const std::string shader_source = glsl_accessors + main_glsl;
  /* Build shader identifier only from user glsl sources (not 100% fiable) */
  Scene *scene = DEG_get_input_scene(depsgraph);
  int normals_domain_val = (mesh_eval->normals_domain() == blender::bke::MeshNormalDomain::Face) ?
                               1 :
                               0;
  int normals_hq_val = int(bool(scene->r.perf_flag & SCE_PERF_HQ_NORMALS) ||
                           GPU_use_hq_normals_workaround());

  std::string shader_key_src;
  shader_key_src.reserve(shader_source.size() + 128);
  shader_key_src = shader_source;
  shader_key_src += "|nd=" + std::to_string(normals_domain_val);
  shader_key_src += "|nhq=" + std::to_string(normals_hq_val);
  const size_t shader_key = std::hash<std::string>()(shader_key_src);

  /* Lookup existing shader for this mesh + variant. */
  blender::gpu::Shader *shader = nullptr;
  auto it_shader = mesh_data.compute_shaders.find(shader_key);
  if (it_shader != mesh_data.compute_shaders.end()) {
    shader = it_shader->second;
  }
  else {
    using namespace blender::gpu::shader;
    ShaderCreateInfo info("pyGPU_Shader");
    info.local_group_size(256, 1, 1);
    info.compute_source("draw_colormanagement_lib.glsl");
    info.compute_source_generated = shader_source;

    /* User buffer bindings (use local_bindings which may contain injected defaults). */
    for (const auto &binding : local_bindings) {
      info.storage_buf(binding.binding, binding.qualifiers, binding.type_name, binding.bind_name);
    }

    /* Topology buffer binding */
    info.storage_buf(MESH_GPU_TOPOLOGY_BINDING, Qualifier::read, "int", "topo[]");

    /* Builtin Specialization constants */
    Scene *scene = DEG_get_input_scene(depsgraph);
    int normals_domain_val = (mesh_eval->normals_domain() ==
                              blender::bke::MeshNormalDomain::Face) ?
                                 1 :
                                 0;
    int normals_hq_val = int(bool(scene->r.perf_flag & SCE_PERF_HQ_NORMALS) ||
                             GPU_use_hq_normals_workaround());

    info.specialization_constant(Type::int_t, "normals_domain", normals_domain_val);
    info.specialization_constant(Type::int_t, "normals_hq", normals_hq_val);

    BKE_mesh_gpu_topology_add_specialization_constants(info, mesh_data.topology);

    /* User Specialization constants (and push_constants) */
    if (config_fn) {
      config_fn(info);
    }

    /* Cast is needed because ShaderCreateInfo is a C++ wrapper; assume compatible here. */
    shader = GPU_shader_create_from_info_python((GPUShaderCreateInfo *)&info, false);
    if (!shader) {
      return blender::bke::GpuComputeStatus::Error;
    }
    mesh_data.compute_shaders.emplace(shader_key, shader);
  }

  /* Bind shader, bind buffers, update uniforms, and compute */
  const blender::gpu::shader::SpecializationConstants *constants =
      &GPU_shader_get_default_constant_state(shader);
  GPU_shader_bind(shader, constants);

  /* Use local_bindings for actual binding as well. */
  for (const auto &binding : local_bindings) {
    std::visit(
        [&](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, blender::gpu::StorageBuf *>) {
            if (arg) {
              GPU_storagebuf_bind(arg, binding.binding);
            }
          }
          else if constexpr (std::is_same_v<T, blender::gpu::VertBuf *>) {
            if (arg) {
              arg->bind_as_ssbo(binding.binding);
            }
          }
          else if constexpr (std::is_same_v<T, blender::gpu::UniformBuf *>) {
            if (arg) {
              GPU_uniformbuf_bind_as_ssbo(arg, binding.binding);
            }
          }
          else if constexpr (std::is_same_v<T, blender::gpu::IndexBuf *>) {
            if (arg) {
              GPU_indexbuf_bind_as_ssbo(arg, binding.binding);
            }
          }
        },
        binding.buffer);
  }

  GPU_storagebuf_bind(mesh_data.topology.ssbo, MESH_GPU_TOPOLOGY_BINDING);
  /* Allow caller to set runtime push-constants / uniforms after shader is bound
   * and before the dispatch. */
  if (post_bind_fn) {
    post_bind_fn(shader);
  }
  const int group_size = 256;
  const int num_groups = (dispatch_count + group_size - 1) / group_size;
  GPU_compute_dispatch(shader, num_groups, 1, 1, constants);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_VERTEX_ATTRIB_ARRAY);
  GPU_shader_unbind();

  if (ob_orig) {
    DEG_id_tag_update(&ob_orig->id, ID_RECALC_TRANSFORM);
  }

  return blender::bke::GpuComputeStatus::Success;
}

blender::bke::GpuComputeStatus BKE_mesh_gpu_scatter_to_corners(
    const Depsgraph *depsgraph,
    const Object *ob_eval,
    const std::vector<blender::bke::GpuMeshComputeBinding> &caller_bindings,
    const std::function<void(blender::gpu::shader::ShaderCreateInfo &)> &config_fn,
    const std::function<void(blender::gpu::Shader *)> &post_bind_fn,
    int dispatch_count)
{
  return BKE_mesh_gpu_run_compute(depsgraph,
                                  ob_eval,
                                  scatter_to_corners_main_glsl,
                                  caller_bindings,
                                  config_fn,
                                  post_bind_fn,
                                  dispatch_count);
}

void BKE_mesh_gpu_free_for_mesh(Mesh *mesh)
{
  if (mesh == nullptr) {
    return;
  }

  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
  auto it = MeshGPUCacheManager::get().mesh_cache().find(mesh);
  if (it == MeshGPUCacheManager::get().mesh_cache().end()) {
    /* Ensure flag reset even if no cached data */
    mesh->is_running_gpu_animation_playback = 0;
    return;
  }

  /* Move data out of the cache map. */
  MeshGpuData data = std::move(it->second);
  MeshGPUCacheManager::get().mesh_cache().erase(it);

  if (GPU_context_active_get()) {
    /* Immediate GPU-safe deletion. */
    for (auto &pair : data.compute_shaders) {
      if (pair.second) {
        GPU_shader_free(pair.second);
        pair.second = nullptr;
      }
    }
    data.compute_shaders.clear();
    if (data.internal_resources) {
      for (auto *ssbo : data.internal_resources->ssbos) {
        if (ssbo) {
          GPU_storagebuf_free(ssbo);
        }
      }
      for (auto *vbo : data.internal_resources->vbos) {
        if (vbo) {
          GPU_vertbuf_clear(vbo);
        }
      }
      for (auto *ib : data.internal_resources->ibos) {
        if (ib) {
          /* Index buffer cleanup handled by GPU module if needed */
        }
      }
      for (auto *ub : data.internal_resources->ubos) {
        if (ub) {
          /* Uniform buffer cleanup handled by GPU module if needed */
        }
      }
      for (auto *sh : data.internal_resources->shaders) {
        if (sh) {
          GPU_shader_free(sh);
        }
      }
      delete data.internal_resources;
      data.internal_resources = nullptr;
    }
    BKE_mesh_gpu_topology_free(data.topology);
  }
  else {
    /* Defer freeing until a GPU context is available. */
    MeshGPUCacheManager::get().orphans().push_back(std::move(data));
  }

  mesh->is_running_gpu_animation_playback = 0;
}

MeshGpuInternalResources *BKE_mesh_gpu_internal_resources_ensure(Mesh *mesh)
{
  if (!mesh) {
    return nullptr;
  }
  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
  auto &d = MeshGPUCacheManager::get().mesh_cache()[mesh];
  if (!d.internal_resources) {
    d.internal_resources = new blender::bke::MeshGpuInternalResources();
  }
  return d.internal_resources;
}

void BKE_mesh_gpu_internal_resources_free_for_mesh(Mesh *mesh)
{
  if (!mesh) {
    return;
  }
  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
  auto it = MeshGPUCacheManager::get().mesh_cache().find(mesh);
  if (it == MeshGPUCacheManager::get().mesh_cache().end()) {
    return;
  }
  MeshGpuData &d = it->second;
  if (!d.internal_resources) {
    return;
  }
  if (GPU_context_active_get()) {
    for (auto *ssbo : d.internal_resources->ssbos) {
      if (ssbo) {
        GPU_storagebuf_free(ssbo);
      }
    }
    for (auto *vbo : d.internal_resources->vbos) {
      if (vbo) {
        GPU_vertbuf_clear(vbo);
      }
    }
    for (auto *sh : d.internal_resources->shaders) {
      if (sh) {
        GPU_shader_free(sh);
      }
    }
    delete d.internal_resources;
    d.internal_resources = nullptr;
  }
  else {
    MeshGPUCacheManager::get().orphans().push_back(MeshGpuData());
    // move internals to the orphan entry we just appended
    MeshGpuData &orphan = MeshGPUCacheManager::get().orphans().back();
    orphan.internal_resources = d.internal_resources;
    orphan.topology = std::move(d.topology);
    orphan.compute_shaders = std::move(d.compute_shaders);
    d.internal_resources = nullptr;
    d.compute_shaders.clear();
  }
}

blender::gpu::Shader *BKE_mesh_gpu_internal_shader_ensure(
    Mesh *mesh, const std::string &key, const blender::gpu::shader::ShaderCreateInfo &info)
{
  if (!mesh) {
    return nullptr;
  }
  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
  MeshGpuData &d = MeshGPUCacheManager::get().mesh_cache()[mesh];
  if (!d.internal_resources) {
    d.internal_resources = new blender::bke::MeshGpuInternalResources();
  }
  auto it = d.internal_resources->shader_map.find(key);
  if (it != d.internal_resources->shader_map.end()) {
    it->second.second += 1;
    return it->second.first;
  }
  /* Create shader (must be called with GL context active). */
  if (!GPU_context_active_get()) {
    return nullptr;
  }
  /* Cast is needed because ShaderCreateInfo is a C++ wrapper; assume compatible here. */
  blender::gpu::Shader *sh = GPU_shader_create_from_info_python((GPUShaderCreateInfo *)&info, false);
  if (!sh) {
    return nullptr;
  }
  d.internal_resources->shader_map.emplace(key, std::make_pair(sh, 1));
  d.internal_resources->shaders.append(sh);
  return sh;
}

blender::gpu::StorageBuf *BKE_mesh_gpu_internal_ssbo_ensure(Mesh *mesh,
                                                            const std::string &key,
                                                            size_t size)
{
  if (!mesh) {
    return nullptr;
  }
  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
  MeshGpuData &d = MeshGPUCacheManager::get().mesh_cache()[mesh];
  if (!d.internal_resources) {
    d.internal_resources = new blender::bke::MeshGpuInternalResources();
  }
  auto it = d.internal_resources->ssbo_map.find(key);
  if (it != d.internal_resources->ssbo_map.end()) {
    it->second.second += 1;
    return it->second.first;
  }
  if (!GPU_context_active_get()) {
    return nullptr;
  }
  blender::gpu::StorageBuf *buf = GPU_storagebuf_create(size);
  if (!buf) {
    return nullptr;
  }
  d.internal_resources->ssbo_map.emplace(key, std::make_pair(buf, 1));
  d.internal_resources->ssbos.append(buf);
  return buf;
}

void BKE_mesh_gpu_internal_shader_release(Mesh *mesh, const std::string &key)
{
  if (!mesh) {
    return;
  }
  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
  auto it = MeshGPUCacheManager::get().mesh_cache().find(mesh);
  if (it == MeshGPUCacheManager::get().mesh_cache().end()) {
    return;
  }
  MeshGpuData &d = it->second;
  if (!d.internal_resources) {
    return;
  }
  auto it2 = d.internal_resources->shader_map.find(key);
  if (it2 == d.internal_resources->shader_map.end()) {
    return;
  }
  auto &entry = it2->second;
  entry.second -= 1;
  if (entry.second <= 0) {
    if (entry.first && GPU_context_active_get()) {
      GPU_shader_free(entry.first);
    }
    /* remove from vector if present */
    for (int i = 0; i < d.internal_resources->shaders.size(); ++i) {
      if (d.internal_resources->shaders[i] == entry.first) {
        d.internal_resources->shaders.remove(i);
        break;
      }
    }
    d.internal_resources->shader_map.erase(it2);
  }
}

blender::gpu::StorageBuf *BKE_mesh_gpu_internal_ssbo_get(Mesh *mesh, const std::string &key)
{
  if (!mesh) {
    return nullptr;
  }
  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
  auto it = MeshGPUCacheManager::get().mesh_cache().find(mesh);
  if (it == MeshGPUCacheManager::get().mesh_cache().end()) {
    return nullptr;
  }
  MeshGpuData &d = it->second;
  if (!d.internal_resources) {
    return nullptr;
  }
  auto it2 = d.internal_resources->ssbo_map.find(key);
  if (it2 == d.internal_resources->ssbo_map.end()) {
    return nullptr;
  }
  return it2->second.first;
}

void BKE_mesh_gpu_internal_ssbo_release(Mesh *mesh, const std::string &key)
{
  if (!mesh) {
    return;
  }
  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
  auto it = MeshGPUCacheManager::get().mesh_cache().find(mesh);
  if (it == MeshGPUCacheManager::get().mesh_cache().end()) {
    return;
  }
  MeshGpuData &d = it->second;
  if (!d.internal_resources) {
    return;
  }
  auto it2 = d.internal_resources->ssbo_map.find(key);
  if (it2 == d.internal_resources->ssbo_map.end()) {
    return;
  }
  auto &entry = it2->second;
  entry.second -= 1;
  if (entry.second <= 0) {
    if (entry.first && GPU_context_active_get()) {
      GPU_storagebuf_free(entry.first);
    }
    /* remove from vector if present */
    for (int i = 0; i < d.internal_resources->ssbos.size(); ++i) {
      if (d.internal_resources->ssbos[i] == entry.first) {
        d.internal_resources->ssbos.remove(i);
        break;
      }
    }
    d.internal_resources->ssbo_map.erase(it2);
  }
}
/* Armature resource helpers. These are simple wrappers reusing MeshGpuInternalResources so we
 * can store SSBOs/shaders keyed by armature object pointer. */
blender::gpu::StorageBuf *BKE_armature_gpu_internal_ssbo_ensure(Object *arm,
                                                                const std::string &key,
                                                                size_t size)
{
  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
  auto &arm_res_map = MeshGPUCacheManager::get().armature_resources();
  auto &res = arm_res_map[arm];
  auto it = res.ssbo_map.find(key);
  if (it != res.ssbo_map.end()) {
    it->second.second += 1;
    return it->second.first;
  }
  if (!GPU_context_active_get()) {
    return nullptr;
  }
  blender::gpu::StorageBuf *buf = GPU_storagebuf_create(size);
  if (!buf) {
    return nullptr;
  }
  res.ssbo_map.emplace(key, std::make_pair(buf, 1));
  res.ssbos.append(buf);
  return buf;
}

blender::gpu::StorageBuf *BKE_armature_gpu_internal_ssbo_get(Object *arm, const std::string &key)
{
  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
  auto &arm_res_map = MeshGPUCacheManager::get().armature_resources();
  auto it = arm_res_map.find(arm);
  if (it == arm_res_map.end()) {
    return nullptr;
  }
  auto &res = it->second;
  auto it2 = res.ssbo_map.find(key);
  if (it2 == res.ssbo_map.end()) {
    return nullptr;
  }
  return it2->second.first;
}

void BKE_armature_gpu_internal_ssbo_release(Object *arm, const std::string &key)
{
  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
  auto &arm_res_map = MeshGPUCacheManager::get().armature_resources();
  auto it = arm_res_map.find(arm);
  if (it == arm_res_map.end()) {
    return;
  }
  auto &res = it->second;
  auto it2 = res.ssbo_map.find(key);
  if (it2 == res.ssbo_map.end()) {
    return;
  }
  auto &entry = it2->second;
  entry.second -= 1;
  if (entry.second <= 0) {
    if (entry.first && GPU_context_active_get()) {
      GPU_storagebuf_free(entry.first);
    }
    /* remove from vector if present */
    for (int i = 0; i < res.ssbos.size(); ++i) {
      if (res.ssbos[i] == entry.first) {
        res.ssbos.remove(i);
        break;
      }
    }
    res.ssbo_map.erase(it2);
  }
}

void BKE_armature_gpu_internal_free_all_armature_caches()
{
  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
  auto &arm_res_map = MeshGPUCacheManager::get().armature_resources();
  if (GPU_context_active_get()) {
    for (auto &kv : arm_res_map) {
      auto &res = kv.second;
      for (auto *ssbo : res.ssbos) {
        if (ssbo) {
          GPU_storagebuf_free(ssbo);
        }
      }
      for (auto *sh : res.shaders) {
        if (sh) {
          GPU_shader_free(sh);
        }
      }
    }
    arm_res_map.clear();
  }
  else {
    /* Move to orphans map? For simplicity, rely on GPU module cleanup. */
    arm_res_map.clear();
  }
}

void BKE_mesh_gpu_free_all_caches()
{
  /* Capture context state early so we can safely release the mutex before calling
   * functions that may need the same mutex internally. */
  const bool has_ctx = GPU_context_active_get();

  {
    std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());

    if (has_ctx) {
      /* Free mesh scoped resources now. */
      for (auto &kv : MeshGPUCacheManager::get().mesh_cache()) {
        MeshGpuData &d = kv.second;
        for (auto &pair : d.compute_shaders) {
          if (pair.second) {
            GPU_shader_free(pair.second);
            pair.second = nullptr;
          }
        }
        d.compute_shaders.clear();

        if (d.internal_resources) {
          for (auto *ssbo : d.internal_resources->ssbos) {
            if (ssbo) {
              GPU_storagebuf_free(ssbo);
            }
          }
          for (auto *vbo : d.internal_resources->vbos) {
            if (vbo) {
              GPU_vertbuf_clear(vbo);
            }
          }
          for (auto *ib : d.internal_resources->ibos) {
            if (ib) {
              /* Index buffer cleanup handled by GPU module if needed */
            }
          }
          for (auto *ub : d.internal_resources->ubos) {
            if (ub) {
              /* Uniform buffer cleanup handled by GPU module if needed */
            }
          }
          for (auto *sh : d.internal_resources->shaders) {
            if (sh) {
              GPU_shader_free(sh);
            }
          }
          delete d.internal_resources;
          d.internal_resources = nullptr;
        }

        BKE_mesh_gpu_topology_free(d.topology);
      }
      MeshGPUCacheManager::get().mesh_cache().clear();

      /* NOTE: don't call armature/more complex frees while holding the mutex –
       * these functions may take the same mutex internally. They are called after
       * the lock scope below. */
    }
    else {
      /* Move all mesh data to orphans to be freed when a GL context becomes available. */
      for (auto &kv : MeshGPUCacheManager::get().mesh_cache()) {
        MeshGPUCacheManager::get().orphans().push_back(std::move(kv.second));
      }
      MeshGPUCacheManager::get().mesh_cache().clear();

      /* Armature resources: rely on GPU module cleanup or later explicit free. Clear map to drop
       * references. */
      MeshGPUCacheManager::get().armature_resources().clear();
    }
  }

  /* Call free functions that may lock g_mesh_cache_mutex. Do this outside the previous lock scope
   * to avoid deadlocks. */
  if (has_ctx) {
    /* free armature scoped resources too */
    BKE_armature_gpu_internal_free_all_armature_caches();

    /* Flush orphans now that context is active. */
    MeshGPUCacheManager::get().flush_orphans();
  }
}
