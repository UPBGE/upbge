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

blender::bke::MeshGpuData *BKE_mesh_gpu_ensure_data(struct Mesh *mesh_orig, struct Mesh *mesh_eval)
{
  if (!mesh_orig || !mesh_eval) {
    return nullptr;
  }

  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());

  auto &mesh_data = MeshGPUCacheManager::get().mesh_cache()[mesh_orig];

  /* Create/upload topology (from evaluated mesh) if needed. */
  if (!mesh_data.topology.ssbo) {
    if (!BKE_mesh_gpu_topology_create(mesh_eval, mesh_data.topology) ||
        !BKE_mesh_gpu_topology_upload(mesh_data.topology))
    {
      return nullptr;
    }
  }

  /* Ensure internal resources container exists. */
  if (!mesh_data.internal_resources) {
    mesh_data.internal_resources = new blender::bke::MeshGpuInternalResources();
    mesh_data.session_uid = mesh_orig->id.session_uid;
  }

  return &mesh_data;
}

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
    if (d.internal_resources) {
      for (auto *ssbo : d.internal_resources->ssbos) {
        if (ssbo) {
          GPU_storagebuf_free(ssbo);
        }
      }
      for (auto *ubo : d.internal_resources->ubos) {
        if (ubo) {
          GPU_uniformbuf_free(ubo);
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

blender::bke::MeshGPUTopology *BKE_mesh_gpu_get_topology(Mesh *mesh)
{
  if (!mesh) {
    return nullptr;
  }
  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
  auto it = MeshGPUCacheManager::get().mesh_cache().find(mesh);
  if (it == MeshGPUCacheManager::get().mesh_cache().end()) {
    return nullptr;
  }
  return &it->second.topology;
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

void main() {
  uint c = gl_GlobalInvocationID.x;
  if (c >= positions_out.length()) {
    return;
  }

  int v = corner_verts(int(c));

  // 1) Scatter position (already in mesh space from skinning)
  vec4 p_mesh = positions_in[v];
  positions_out[c] = p_mesh;

  // 2) Calculate and scatter normal
  vec3 n_mesh;
  if (normals_domain == 1) { // Face
    int f = corner_to_face(int(c));
    n_mesh = newell_face_normal_object(f);
  }
  else { // Point
    int beg = vert_to_face_offsets(v);
    int end = vert_to_face_offsets(v + 1);
    vec3 n_accum = vec3(0.0);
    for (int i = beg; i < end; ++i) {
      int f = vert_to_face(i);
      n_accum += newell_face_normal_object(f);
    }
    n_mesh = n_accum;
  }

  // Normal already in mesh space (no transformation needed)
  n_mesh = normalize(n_mesh);

  if (normals_hq == 0) {
    normals_out[c] = pack_norm(n_mesh);
  }
  else {
    int base = int(c) * 2;
    normals_out[base + 0] = pack_i16_pair(n_mesh.x, n_mesh.y);
    normals_out[base + 1] = pack_i16_pair(n_mesh.z, 0.0);
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
static bool has_bind_name(const char *name,
                          blender::Span<blender::bke::GpuMeshComputeBinding> local_bindings)
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
static int find_free_binding(blender::Span<blender::bke::GpuMeshComputeBinding> local_bindings,
                             int start = 0)
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
    blender::Span<blender::bke::GpuMeshComputeBinding> caller_bindings,
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
    /* Position VBO has wrong stride (expected vec4 = 16 bytes).
     * Request geometry recalc to force correct extraction format. */
    BKE_mesh_gpu_free_for_mesh(mesh_orig);
    /* Skip BKE_mesh_batch_cache_dirty_tag but recontruct mesh runtime draw cache on next frame */
    BKE_mesh_request_gpu_render_cache_update(mesh_orig, mesh_eval, ob_orig);
    return blender::bke::GpuComputeStatus::NotReady;
  }

  blender::bke::MeshGpuData *mesh_data_ptr = BKE_mesh_gpu_ensure_data(mesh_orig, mesh_eval);
  if (!mesh_data_ptr) {
    BKE_mesh_gpu_free_for_mesh(mesh_orig);
    return blender::bke::GpuComputeStatus::Error;
  }
  auto &mesh_data = *mesh_data_ptr;

  /* --- Prepare bindings vector, inject defaults for scatter shader if needed --- */
  blender::Vector<blender::bke::GpuMeshComputeBinding> local_bindings;
  local_bindings.reserve(caller_bindings.size() + 4);
  for (const auto &b : caller_bindings) {
    local_bindings.append(b);
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
        auto *entry_ptr = mesh_data.internal_resources->ssbo_map.lookup_ptr(key);
        if (entry_ptr) {
          ssbo = entry_ptr->buffer;
        }
      }

      if (!ssbo) {
        /* positions_in always aligned on verts_num */
        const int verts = mesh_eval->verts_num;
        if (verts > 0 && GPU_context_active_get()) {
          const size_t size_bytes = size_t(verts) * sizeof(float) * 4; /* vec4 per vertex */

          /* Build CPU buffer OUTSIDE the mutex to avoid holding the lock during heavy work. */
          /* No mutex held here: BKE_mesh_gpu_internal_ssbo_ensure acquires it internally. */
          blender::Vector<blender::float4> pos_data;
          pos_data.resize(size_t(verts));
          blender::Span<blender::float3> pos_span = mesh_eval->vert_positions();
          blender::threading::parallel_for(
              blender::IndexRange(verts), 4096, [&](blender::IndexRange range) {
                for (int i : range) {
                  pos_data[i] = blender::float4(pos_span[i].xyz(), 1.0f);
                }
              });

          /* Ensure (may return existing SSBO if another thread created it meanwhile). */
          ssbo = BKE_mesh_gpu_internal_ssbo_ensure(mesh_eval, key, size_bytes);
          if (ssbo) {
            GPU_storagebuf_update(ssbo, pos_data.data());
          }
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
        local_bindings.append(gb);
      }
    }

    /* Create default transform_mat SSBO with identity matrix if missing. */
    if (!has_transform_mat) {
      if (GPU_context_active_get()) {
        const std::string key = "scatter_transform_mat";
        float mat[4][4];
        unit_m4(mat);
        /* No mutex held here: BKE_mesh_gpu_internal_ssbo_ensure acquires it internally. */
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
            local_bindings.append(gb);
          }
        }
        catch (...) {
          throw;
        }
        /* continue */
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
            local_bindings.append(gb);
          }
        }
      }
    }
    if (!has_normals_out) {
      if (cache) {
        if (auto *nor_ptr = cache->final.buff.vbos.lookup_ptr(
                blender::draw::VBOType::CornerNormal))
        {
          blender::gpu::VertBuf *vbo_nor = nor_ptr->get();
          if (vbo_nor) {
            blender::bke::GpuMeshComputeBinding gb = {};
            gb.binding = find_free_binding(local_bindings, 0);
            gb.buffer = vbo_nor;
            gb.qualifiers = blender::gpu::shader::Qualifier::write;
            gb.type_name = "uint";
            gb.bind_name = "normals_out[]";
            local_bindings.append(gb);
          }
        }
      }
    }
    /* Caller is using scatter_to_corners -> dispatch count set automatically to corners_num */
    dispatch_count = mesh_eval->corners_num;
  }

  std::string glsl_accessors = BKE_mesh_gpu_topology_glsl_accessors_string(mesh_data.topology);
  const std::string shader_source = glsl_accessors + main_glsl;
  /* Build shader identifier */
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
  const size_t shader_hash = std::hash<std::string>()(shader_key_src);
  const std::string shader_key = std::to_string(shader_hash);

  /* Lookup existing shader for this mesh + variant in internal resources. */
  blender::gpu::Shader *shader = nullptr;
  if (mesh_data.internal_resources) {
    auto *shader_entry = mesh_data.internal_resources->shader_map.lookup_ptr(shader_key);
    if (shader_entry) {
      shader = shader_entry->shader;
    }
  }

  if (!shader) {
    using namespace blender::gpu::shader;
    ShaderCreateInfo info("pyGPU_Shader");
    info.local_group_size(256, 1, 1);
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
    /* Store shader in internal resources (create container if needed). */
    if (!mesh_data.internal_resources) {
      mesh_data.internal_resources = new blender::bke::MeshGpuInternalResources();
    }
    mesh_data.internal_resources->shader_map.add_new(shader_key, {shader, 1});
    mesh_data.internal_resources->shaders.append(shader);
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
    blender::Span<blender::bke::GpuMeshComputeBinding> caller_bindings,
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
      for (auto *ubo : data.internal_resources->ubos) {
        if (ubo) {
          GPU_uniformbuf_free(ubo);
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

/* Always return after! (wait for the next frame) */
void BKE_mesh_request_gpu_render_cache_update(Mesh *mesh_orig, Mesh *mesh_eval, Object *ob_orig)
{
  /* Set playback flag to skip CPU modifier stack and preserve mesh_eval.
   *
   * When this flag is set:
   * - BKE_object_batch_cache_dirty_tag() skips batch_cache invalidation
   * - Mesh_eval is NOT freed (unlike normal ID_RECALC_GEOMETRY)
   * - VBO extraction will use vec4 positions (stride = 16)
   */
  mesh_orig->is_running_gpu_animation_playback = 1;

  if (mesh_eval) {
    mesh_eval->is_running_gpu_animation_playback = 1;
  }

  /* Tag depsgraph to trigger geometry update.
   *
   * This will:
   * - Trigger VBO extraction with correct stride
   * - Update draw cache (but NOT invalidate batch_cache due to flag above)
   */
  DEG_id_tag_update(&ob_orig->id, ID_RECALC_GEOMETRY);

  /* Notify viewport to redraw (will be done on next frame). */
  WM_main_add_notifier(NC_WINDOW, nullptr);
}

MeshGpuInternalResources *BKE_mesh_gpu_internal_resources_ensure(Mesh *mesh)
{
  if (!mesh) {
    return nullptr;
  }
  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
  auto &d = MeshGPUCacheManager::get().mesh_cache()[mesh];

  /* Initialize session UID on first access (for validation) */
  if (d.session_uid == 0) {
    d.session_uid = mesh->id.session_uid;
  }

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
    for (auto *ubo : d.internal_resources->ubos) {
      if (ubo) {
        GPU_uniformbuf_free(ubo);
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
    d.internal_resources = nullptr;
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

  /* Initialize session UID on first access (for validation) */
  if (d.session_uid == 0) {
    d.session_uid = mesh->id.session_uid;
  }

  if (!d.internal_resources) {
    d.internal_resources = new blender::bke::MeshGpuInternalResources();
  }
  auto *entry_ptr = d.internal_resources->shader_map.lookup_ptr(key);
  if (entry_ptr) {
    entry_ptr->refcount += 1;
    return entry_ptr->shader;
  }
  /* Create shader (must be called with GL context active). */
  if (!GPU_context_active_get()) {
    return nullptr;
  }
  /* Cast is needed because ShaderCreateInfo is a C++ wrapper; assume compatible here. */
  blender::gpu::Shader *sh = GPU_shader_create_from_info_python((GPUShaderCreateInfo *)&info,
                                                                false);
  if (!sh) {
    return nullptr;
  }
  d.internal_resources->shader_map.add_new(key, {sh, 1});
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
  auto *entry_ptr = d.internal_resources->ssbo_map.lookup_ptr(key);
  if (entry_ptr) {
    entry_ptr->refcount += 1;
    return entry_ptr->buffer;
  }
  if (!GPU_context_active_get()) {
    return nullptr;
  }
  blender::gpu::StorageBuf *buf = GPU_storagebuf_create(size);
  GPU_storagebuf_clear_to_zero(buf);
  if (!buf) {
    return nullptr;
  }
  d.internal_resources->ssbo_map.add_new(key, {buf, 1});
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
  auto *entry_ptr = d.internal_resources->shader_map.lookup_ptr(key);
  if (!entry_ptr) {
    return;
  }
  entry_ptr->refcount -= 1;
  if (entry_ptr->refcount <= 0) {
    if (entry_ptr->shader && GPU_context_active_get()) {
      GPU_shader_free(entry_ptr->shader);
    }
    /* remove from vector if present */
    for (int i = 0; i < d.internal_resources->shaders.size(); ++i) {
      if (d.internal_resources->shaders[i] == entry_ptr->shader) {
        d.internal_resources->shaders.remove(i);
        break;
      }
    }
    d.internal_resources->shader_map.remove(key);
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
  auto *entry_ptr = d.internal_resources->ssbo_map.lookup_ptr(key);
  if (!entry_ptr) {
    return nullptr;
  }
  return entry_ptr->buffer;
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
  auto *entry_ptr = d.internal_resources->ssbo_map.lookup_ptr(key);
  if (!entry_ptr) {
    return;
  }
  entry_ptr->refcount -= 1;
  if (entry_ptr->refcount <= 0) {
    if (entry_ptr->buffer && GPU_context_active_get()) {
      GPU_storagebuf_free(entry_ptr->buffer);
    }
    /* remove from vector if present */
    for (int i = 0; i < d.internal_resources->ssbos.size(); ++i) {
      if (d.internal_resources->ssbos[i] == entry_ptr->buffer) {
        d.internal_resources->ssbos.remove(i);
        break;
      }
    }
    d.internal_resources->ssbo_map.remove(key);
  }
}

/* -------------------------------------------------------------------- */
/** \name UBO Cache Management (same pattern as SSBO)
 * \{ */

blender::gpu::UniformBuf *BKE_mesh_gpu_internal_ubo_ensure(Mesh *mesh,
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
  auto *entry_ptr = d.internal_resources->ubo_map.lookup_ptr(key);
  if (entry_ptr) {
    entry_ptr->refcount += 1;
    return entry_ptr->buffer;
  }
  if (!GPU_context_active_get()) {
    return nullptr;
  }
  blender::gpu::UniformBuf *buf = GPU_uniformbuf_create(size);
  if (!buf) {
    return nullptr;
  }
  d.internal_resources->ubo_map.add_new(key, {buf, 1});
  d.internal_resources->ubos.append(buf);
  return buf;
}

blender::gpu::UniformBuf *BKE_mesh_gpu_internal_ubo_get(Mesh *mesh, const std::string &key)
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
  auto *entry_ptr = d.internal_resources->ubo_map.lookup_ptr(key);
  if (!entry_ptr) {
    return nullptr;
  }
  return entry_ptr->buffer;
}

void BKE_mesh_gpu_internal_ubo_release(Mesh *mesh, const std::string &key)
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
  auto *entry_ptr = d.internal_resources->ubo_map.lookup_ptr(key);
  if (!entry_ptr) {
    return;
  }
  entry_ptr->refcount -= 1;
  if (entry_ptr->refcount <= 0) {
    if (entry_ptr->buffer && GPU_context_active_get()) {
      GPU_uniformbuf_free(entry_ptr->buffer);
    }
    /* remove from vector if present */
    for (int i = 0; i < d.internal_resources->ubos.size(); ++i) {
      if (d.internal_resources->ubos[i] == entry_ptr->buffer) {
        d.internal_resources->ubos.remove(i);
        break;
      }
    }
    d.internal_resources->ubo_map.remove(key);
  }
}

/** \} */

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
          for (auto *ubo : d.internal_resources->ubos) {
            if (ubo) {
              GPU_uniformbuf_free(ubo);
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

      /* NOTE: don't call more complex frees while holding the mutex –
       * these functions may take the same mutex internally. They are called after
       * the lock scope below. */
    }
    else {
      /* Move all mesh data to orphans to be freed when a GL context becomes available. */
      for (auto &kv : MeshGPUCacheManager::get().mesh_cache()) {
        MeshGPUCacheManager::get().orphans().push_back(std::move(kv.second));
      }
      MeshGPUCacheManager::get().mesh_cache().clear();
    }
  }

  /* Call free functions that may lock g_mesh_cache_mutex. Do this outside the previous lock scope
   * to avoid deadlocks. */
  if (has_ctx) {
    /* Flush orphans now that context is active. */
    MeshGPUCacheManager::get().flush_orphans();
  }
}
