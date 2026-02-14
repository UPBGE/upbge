/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "mesh_gpu_cache.hh"

#include "BKE_mesh_gpu.hh"

#include <fmt/format.h>
#include <mutex>
#include <unordered_map>
#include <cstdio>

#include "BKE_mesh.hh"
#include "BKE_scene.hh"
#include "BKE_mesh_mapping.hh"

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
#include "../gpu/gpu_deform_common/gpu_shader_common_normal_lib.hh"  /* Common normal calculation functions */

namespace blender {
namespace bke {


using bke::MeshGPUCacheManager;
using bke::MeshGpuData;

/* Helper: free internal resources (must be called without holding cache mutex). */
static void mesh_gpu_free_internal_resources_ptr(bke::MeshGpuInternalResources *ir)
{
  if (!ir) {
    return;
  }
  for (const auto &kv : ir->ssbo_map.items()) {
    auto &entry = kv.value;
    if (entry.buffer) {
      GPU_storagebuf_free(entry.buffer);
    }
  }
  for (const auto &kv : ir->ubo_map.items()) {
    auto &entry = kv.value;
    if (entry.buffer) {
      GPU_uniformbuf_free(entry.buffer);
    }
  }
  for (const auto &kv : ir->vbo_map.items()) {
    auto &entry = kv.value;
    if (entry.buffer) {
      GPU_vertbuf_clear(entry.buffer);
    }
  }
  for (const auto &kv : ir->ibo_map.items()) {
    auto &entry = kv.value;
    if (entry.buffer) {
      GPU_indexbuf_discard(entry.buffer);
    }
  }
  for (const auto &kv : ir->shader_map.items()) {
    auto &entry = kv.value;
    if (entry.shader) {
      GPU_shader_free(entry.shader);
    }
  }
  for (const auto &kv : ir->texture_map.items()) {
    auto &entry = kv.value;
    if (entry.texture) {
      GPU_texture_free(entry.texture);
    }
  }
  delete ir;
}

MeshGpuData *BKE_mesh_gpu_ensure_data(Mesh *mesh_orig, Mesh *mesh_eval)
{
  if (!mesh_orig || !mesh_eval) {
    return nullptr;
  }
  /* Step 1: ensure a cache entry and minimal initialization under the mutex.
   * Heavy work (topology creation/upload) is done outside the mutex to avoid
   * blocking other threads. Use double-checked locking when re-attaching. */
  {
    std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
    auto &mesh_data = MeshGPUCacheManager::get().mesh_cache()[mesh_orig];
    if (mesh_data.session_uid == 0) {
      mesh_data.session_uid = mesh_orig->id.session_uid;
    }
    if (!mesh_data.internal_resources) {
      mesh_data.internal_resources = new bke::MeshGpuInternalResources();
    }
    /* If topology already uploaded by another thread, return it directly. */
    if (mesh_data.topology.ssbo) {
      return &mesh_data;
    }
  }

  /* Step 2: Build and upload topology outside the mutex. */
  bke::MeshGPUTopology tmp_topo;
  if (!BKE_mesh_gpu_topology_create(mesh_eval, tmp_topo)) {
    return nullptr;
  }
  if (!GPU_context_active_get()) {
    BKE_mesh_gpu_topology_free(tmp_topo);
    return nullptr;
  }
  if (!BKE_mesh_gpu_topology_upload(tmp_topo)) {
    BKE_mesh_gpu_topology_free(tmp_topo);
    return nullptr;
  }

  /* Step 3: Re-lock and attach the uploaded topology if not already present. */
  {
    std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
    auto &mesh_data = MeshGPUCacheManager::get().mesh_cache()[mesh_orig];
    if (!mesh_data.topology.ssbo) {
      mesh_data.topology = std::move(tmp_topo);
    }
    else {
      /* Another thread attached a topology while we were building: free ours. */
      BKE_mesh_gpu_topology_free(tmp_topo);
    }
    if (mesh_data.session_uid == 0) {
      mesh_data.session_uid = mesh_orig->id.session_uid;
    }
    if (!mesh_data.internal_resources) {
      mesh_data.internal_resources = new bke::MeshGpuInternalResources();
    }
    return &mesh_data;
  }
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
      mesh_gpu_free_internal_resources_ptr(d.internal_resources);
      d.internal_resources = nullptr;
    }
    BKE_mesh_gpu_topology_free(d.topology);
  }
  orphan_list.clear();
}

/* Note: functions now use MeshGPUCacheManager accessors instead of globals. */

bool BKE_mesh_gpu_topology_create(const Mesh *mesh_eval, bke::MeshGPUTopology &topology)
{
  if (!mesh_eval) {
    return false;
  }

  /* Clear any existing data */
  BKE_mesh_gpu_topology_free(topology);

  /* Get mesh topology data */
  const auto face_offsets = mesh_eval->face_offsets();
  const auto corner_to_face = mesh_eval->corner_to_face_map();
  const auto corner_verts_span = mesh_eval->corner_verts();
  const auto corner_tris = mesh_eval->corner_tris();
  const auto corner_tri_faces = mesh_eval->corner_tri_faces();
  const auto edges = mesh_eval->edges();
  const auto corner_edges_span = mesh_eval->corner_edges();

  /* Convert spans to vectors for easier handling */
  Vector<int> corner_verts_vec(corner_verts_span.begin(), corner_verts_span.end());
  Vector<int> corner_tris_flat;
  corner_tris_flat.reserve(corner_tris.size() * 3);
  for (const int3 &tri : corner_tris) {
    corner_tris_flat.append(tri.x);
    corner_tris_flat.append(tri.y);
    corner_tris_flat.append(tri.z);
  }
  Vector<int> corner_tri_faces_vec(corner_tri_faces.begin(), corner_tri_faces.end());

  Vector<int> edges_flat;
  edges_flat.reserve(edges.size() * 2);
  for (const int2 &edge : edges) {
    edges_flat.append(edge.x);
    edges_flat.append(edge.y);
  }

  Vector<int> corner_edges_vec(corner_edges_span.begin(), corner_edges_span.end());

  /* Build vertex->face offsets/indices from corner data (robust builder).
   * Use `build_vert_to_face_map` to produce consistent offsets and indices
   * that match `corner_verts_span` and `mesh_eval->verts_num`. This avoids
   * relying on potentially stale/cached internal spans. */
  Array<int> rebuilt_offsets;
  Array<int> rebuilt_indices;
  bke::mesh::build_vert_to_face_map(face_offsets, corner_verts_span, mesh_eval->verts_num, rebuilt_offsets, rebuilt_indices);

  Vector<int> v2f_offsets(rebuilt_offsets.begin(), rebuilt_offsets.end());
  Vector<int> v2f_indices(rebuilt_indices.begin(), rebuilt_indices.end());

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

bool BKE_mesh_gpu_topology_upload(bke::MeshGPUTopology &topology)
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

void BKE_mesh_gpu_topology_free(bke::MeshGPUTopology &topology)
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
/* --- Bounds helpers --- */
uint float_to_ordered_uint(float f) {
  uint u = floatBitsToUint(f);
  return (u & 0x80000000u) != 0u ? ~u : (u ^ 0x80000000u);
}

void main() {
  uint c = gl_GlobalInvocationID.x;
  uint lid = gl_LocalInvocationID.x;
  bool valid = (c < positions_out.length());

  int v = valid ? corner_verts(int(c)) : 0;

  // 1) Scatter position (already in mesh space from skinning)
  vec4 p_mesh = valid ? positions_in[v] : vec4(0.0);
  if (valid) {
    positions_out[c] = p_mesh;
  }

  // 2) Calculate and scatter normal
  if (valid) {
    vec3 n_mesh;
    if (normals_domain == 1) { // Face
      int f = corner_to_face(int(c));
      n_mesh = face_normal_object(f);
    }
    else { // Point (smooth) - angle-weighted like CPU
      n_mesh = compute_vertex_normal_smooth(v);
    }

    if (normals_hq == 0) {
      normals_out[c] = pack_norm(n_mesh);
    }
    else {
      int base = int(c) * 2;
      normals_out[base + 0] = pack_i16_pair(n_mesh.x, n_mesh.y);
      normals_out[base + 1] = pack_i16_pair(n_mesh.z, 0.0);
    }
  }

#ifdef USE_BOUNDS_REDUCTION
  // 3) Bounds reduction (in-shader, avoids a separate dispatch)
  const float INF = 1.0 / 0.0;
  wg_min[lid] = valid ? p_mesh.xyz : vec3(INF);
  wg_max[lid] = valid ? p_mesh.xyz : vec3(-INF);

  barrier();
  memoryBarrierShared();

  for (uint s = gl_WorkGroupSize.x >> 1; s > 0u; s >>= 1) {
    if (lid < s) {
      wg_min[lid] = min(wg_min[lid], wg_min[lid + s]);
      wg_max[lid] = max(wg_max[lid], wg_max[lid + s]);
    }
    barrier();
    memoryBarrierShared();
  }

  if (lid == 0u) {
    vec3 gmin = wg_min[0];
    vec3 gmax = wg_max[0];
    atomicMin(bounds_out[0], float_to_ordered_uint(gmin.x));
    atomicMin(bounds_out[1], float_to_ordered_uint(gmin.y));
    atomicMin(bounds_out[2], float_to_ordered_uint(gmin.z));
    atomicMax(bounds_out[4], float_to_ordered_uint(gmax.x));
    atomicMax(bounds_out[5], float_to_ordered_uint(gmax.y));
    atomicMax(bounds_out[6], float_to_ordered_uint(gmax.z));
  }
#endif
}
)GLSL";

/* Helper: Build complete scatter shader source with common normal lib */
static std::string get_scatter_shader_source()
{
  using namespace gpu;
  /* Define position buffer macro before including normal lib */
  return "#define POSITION_BUFFER positions_in\n" + blender::gpu::get_common_normal_lib_glsl() +
         scatter_to_corners_main_glsl;
}

std::string BKE_mesh_gpu_topology_glsl_accessors_string(
    const bke::MeshGPUTopology &topology)
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
    gpu::shader::ShaderCreateInfo &info, const bke::MeshGPUTopology &topology)
{
  using namespace gpu::shader;
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

#define MESH_GPU_BOUNDS_BINDING 14
#define MESH_GPU_TOPOLOGY_BINDING 15

/* Helper to check if a bind_name is present (accepts both "name" and "name[]"). */
static bool has_bind_name(const char *name,
                          Span<bke::GpuMeshComputeBinding> local_bindings)
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
static int find_free_binding(Span<bke::GpuMeshComputeBinding> local_bindings,
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

bke::GpuComputeStatus BKE_mesh_gpu_run_compute(
    const Depsgraph *depsgraph,
    const Object *ob_eval,
    const char *main_glsl,
    Span<bke::GpuMeshComputeBinding> caller_bindings,
    const std::function<void(gpu::shader::ShaderCreateInfo &)> &config_fn,
    const std::function<void(gpu::Shader *)> &post_bind_fn,
    int dispatch_count)
{
  if (!GPU_context_active_get() || !depsgraph || !ob_eval || ob_eval->type != OB_MESH) {
    return bke::GpuComputeStatus::Error;
  }

  /* Attempt to free any deferred resources now that we are on a GPU context. */
  if (GPU_context_active_get()) {
    MeshGPUCacheManager::get().flush_orphans();
  }

  Object *ob_orig = DEG_get_original(const_cast<Object *>(ob_eval));
  Mesh *mesh_orig = (Mesh *)(ob_orig->data);
  Mesh *mesh_eval = (Mesh *)(ob_eval->data);
  if (!mesh_eval) {
    BKE_mesh_gpu_free_for_mesh(mesh_orig);
    return bke::GpuComputeStatus::Error;
  }

  if (!ob_orig) {
    BKE_mesh_gpu_free_for_mesh(mesh_orig);
    return bke::GpuComputeStatus::Error;
  }

  if (ob_orig->mode != OB_MODE_OBJECT) {
    // early return when not in object mode
    BKE_mesh_gpu_free_for_mesh(mesh_orig);
    return bke::GpuComputeStatus::NotReady;
  }

  if (!mesh_eval->runtime || !mesh_eval->runtime->batch_cache) {
    // early return
    BKE_mesh_gpu_free_for_mesh(mesh_orig);
    return bke::GpuComputeStatus::NotReady;
  }

  using namespace draw;

  auto *cache = static_cast<draw::MeshBatchCache *>(mesh_eval->runtime->batch_cache);
  auto *vbo_pos_ptr = cache->final.buff.vbos.lookup_ptr(draw::VBOType::Position);
  if (!vbo_pos_ptr) {
    // early return
    BKE_mesh_gpu_free_for_mesh(mesh_orig);
    return bke::GpuComputeStatus::NotReady;
  }
  auto *vbo_pos = vbo_pos_ptr->get();
  const GPUVertFormat *format = GPU_vertbuf_get_format(vbo_pos);

  if (format->stride == 16 && (ob_orig->id.recalc & ID_RECALC_GEOMETRY) != 0) {
    BKE_mesh_gpu_free_for_mesh(mesh_orig);
    return bke::GpuComputeStatus::NotReady;
  }

  if (format->stride != 16) {
    /* Position VBO has wrong stride (expected vec4 = 16 bytes).
     * Request geometry recalc to force correct extraction format. */
    BKE_mesh_gpu_free_for_mesh(mesh_orig);
    /* Skip BKE_mesh_batch_cache_dirty_tag but recontruct mesh runtime draw cache on next frame */
    BKE_mesh_request_gpu_render_cache_update(mesh_orig, mesh_eval, ob_orig);
    return bke::GpuComputeStatus::NotReady;
  }

  bke::MeshGpuData *mesh_data_ptr = BKE_mesh_gpu_ensure_data(mesh_orig, mesh_eval);
  if (!mesh_data_ptr) {
    BKE_mesh_gpu_free_for_mesh(mesh_orig);
    return bke::GpuComputeStatus::Error;
  }
  auto &mesh_data = *mesh_data_ptr;

  /* --- Prepare bindings vector, inject defaults for scatter shader if needed --- */
  Vector<bke::GpuMeshComputeBinding> local_bindings;
  local_bindings.reserve(caller_bindings.size() + 4);
  for (const auto &b : caller_bindings) {
    local_bindings.append(b);
  }

  /* Only special-case the scatter shader. If called via scatter_to_corners, ensure we have
   * `positions_in` and `transform_mat` SSBOs available and declared. */
  const std::string scatter_shader_src = get_scatter_shader_source();
  if (main_glsl && (main_glsl == scatter_to_corners_main_glsl)) {
    const bool has_positions_in = has_bind_name("positions_in", local_bindings);
    const bool has_transform_mat = has_bind_name("transform_mat", local_bindings);

    /* Create default positions_in SSBO from mesh_eval->vert_positions() if missing.
     * Fast-path: check under mutex to avoid expensive work when SSBO already exists.
     * If missing, build CPU buffer outside the mutex, then call ensure (which will
     * safely create-or-return the SSBO). */
    if (!has_positions_in) {
      const std::string key = "scatter_positions_in";
      gpu::StorageBuf *ssbo = nullptr;

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
          /* No mutex held here: bke::BKE_mesh_gpu_internal_ssbo_ensure acquires it internally. */
          Vector<float4> pos_data;
          pos_data.resize(size_t(verts));
          Span<float3> pos_span = mesh_eval->vert_positions();
          threading::parallel_for(
              IndexRange(verts), 4096, [&](IndexRange range) {
                for (int i : range) {
                  pos_data[i] = float4(pos_span[i].xyz(), 1.0f);
                }
              });

          /* Ensure (may return existing SSBO if another thread created it meanwhile). */
          ssbo = bke::BKE_mesh_gpu_internal_ssbo_ensure(mesh_orig, const_cast<Object *>(ob_eval), key, size_bytes);
          if (ssbo) {
            GPU_storagebuf_update(ssbo, pos_data.data());
          }
        }
      }

      /* If we now have an SSBO (existing or newly created), inject binding. */
      if (ssbo) {
        bke::GpuMeshComputeBinding gb;
        gb.binding = find_free_binding(local_bindings, 0);
        gb.buffer = ssbo;
        gb.qualifiers = gpu::shader::Qualifier::read;
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
        /* No mutex held here: bke::BKE_mesh_gpu_internal_ssbo_ensure acquires it internally. */
        gpu::StorageBuf *ssbo = nullptr;
        try {
          ssbo = bke::BKE_mesh_gpu_internal_ssbo_ensure(mesh_orig, const_cast<Object *>(ob_eval), key, sizeof(float) * 16);
          if (ssbo) {
            GPU_storagebuf_update(ssbo, &mat[0][0]);
            bke::GpuMeshComputeBinding gb;
            gb.binding = find_free_binding(local_bindings, 0);
            gb.buffer = ssbo;
            gb.qualifiers = gpu::shader::Qualifier::read;
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
        if (auto *pos_ptr = cache->final.buff.vbos.lookup_ptr(draw::VBOType::Position)) {
          gpu::VertBuf *vbo = pos_ptr->get();
          if (vbo) {
            bke::GpuMeshComputeBinding gb = {};
            gb.binding = find_free_binding(local_bindings, 0);
            gb.buffer = vbo;
            gb.qualifiers = gpu::shader::Qualifier::read_write;
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
                draw::VBOType::CornerNormal))
        {
          gpu::VertBuf *vbo_nor = nor_ptr->get();
          if (vbo_nor) {
            bke::GpuMeshComputeBinding gb = {};
            gb.binding = find_free_binding(local_bindings, 0);
            gb.buffer = vbo_nor;
            gb.qualifiers = gpu::shader::Qualifier::write;
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

  /* --- Bounds SSBO injection for scatter shader --- */
  const bool is_scatter = (main_glsl && main_glsl == scatter_to_corners_main_glsl);
  /* Check if mesh has bounds computation enabled via its flag.
   * Disabled on Metal backend where GPU_storagebuf_read_fast is not implemented. */
  const bool bounds_enabled = is_scatter && (mesh_orig->is_running_gpu_animation_playback != 0) &&
                              (GPU_backend_get_type() != GPU_BACKEND_METAL);
  const std::string bounds_key = "scatter_bounds_out";
  gpu::StorageBuf *bounds_ssbo = bke::BKE_mesh_gpu_internal_ssbo_get(mesh_orig, bounds_key);
  if (!bounds_ssbo) {
    bounds_ssbo = bke::BKE_mesh_gpu_internal_ssbo_ensure(
        mesh_orig, const_cast<Object *>(ob_eval), bounds_key, sizeof(uint) * 8);
  }
  if (bounds_ssbo) {
    bke::GpuMeshComputeBinding gb = {};
    gb.binding = MESH_GPU_BOUNDS_BINDING;
    gb.buffer = bounds_ssbo;
    gb.qualifiers = gpu::shader::Qualifier::read_write;
    gb.type_name = "uint";
    gb.bind_name = "bounds_out[]";
    local_bindings.append(gb);
  }

  std::string glsl_accessors = BKE_mesh_gpu_topology_glsl_accessors_string(mesh_data.topology);
  /* Use concatenated shader source for scatter shader, otherwise use main_glsl as-is */
  std::string shader_source;
  if (main_glsl == scatter_to_corners_main_glsl) {
    /* For the scatter shader include the generated accessors and the scatter
     * main. Do not enable precomputation defines by default in this fork. */
    shader_source = glsl_accessors + scatter_shader_src;
  }
  else {
    shader_source = glsl_accessors + main_glsl;
  }
  /* Build shader identifier */
  Scene *scene = DEG_get_input_scene(depsgraph);
  int normals_domain_val = (mesh_eval->normals_domain() == bke::MeshNormalDomain::Face) ?
                               1 :
                               0;
  int normals_hq_val = int(bool(scene->r.perf_flag & SCE_PERF_HQ_NORMALS) ||
                           GPU_use_hq_normals_workaround());

  std::string shader_key_src;
  shader_key_src.reserve(shader_source.size() + 128);
  shader_key_src = shader_source;
  shader_key_src += "|nd=" + std::to_string(normals_domain_val);
  shader_key_src += "|nhq=" + std::to_string(normals_hq_val);
  shader_key_src += "|bounds=" + std::to_string(int(bounds_enabled));
  const size_t shader_hash = std::hash<std::string>()(shader_key_src);
  const std::string shader_key = std::to_string(shader_hash);

  /* Lookup existing shader for this mesh + variant in internal resources. */
  gpu::Shader *shader = BKE_mesh_gpu_internal_shader_get(mesh_orig, shader_key);
  if (!shader) {
    using namespace gpu::shader;
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
                              bke::MeshNormalDomain::Face) ?
                                 1 :
                                 0;
    int normals_hq_val = int(bool(scene->r.perf_flag & SCE_PERF_HQ_NORMALS) ||
                             GPU_use_hq_normals_workaround());

    info.specialization_constant(Type::int_t, "normals_domain", normals_domain_val);
    info.specialization_constant(Type::int_t, "normals_hq", normals_hq_val);
    info.define("USE_BOUNDS_REDUCTION", bounds_enabled ? "1" : "0");
    info.typedef_source_generated = R"GLSL(
        shared vec3 wg_min[256];
        shared vec3 wg_max[256];
    )GLSL";

    BKE_mesh_gpu_topology_add_specialization_constants(info, mesh_data.topology);

    /* User Specialization constants (and push_constants) */
    if (config_fn) {
      config_fn(info);
    }

    shader = BKE_mesh_gpu_internal_shader_ensure(mesh_orig, const_cast<Object *>(ob_eval), shader_key, info);
  }

  if (!shader) {
    return bke::GpuComputeStatus::Error;
  }

  /* Bind shader, bind buffers, update uniforms, and compute */
  const gpu::shader::SpecializationConstants *constants =
      &GPU_shader_get_default_constant_state(shader);
  GPU_shader_bind(shader, constants);

  /* Use local_bindings for actual binding as well. */
  for (const auto &binding : local_bindings) {
    std::visit(
        [&](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, gpu::StorageBuf *>) {
            if (arg) {
              GPU_storagebuf_bind(arg, binding.binding);
            }
          }
          else if constexpr (std::is_same_v<T, gpu::VertBuf *>) {
            if (arg) {
              arg->bind_as_ssbo(binding.binding);
            }
          }
          else if constexpr (std::is_same_v<T, gpu::UniformBuf *>) {
            if (arg) {
              GPU_uniformbuf_bind_as_ssbo(arg, binding.binding);
            }
          }
          else if constexpr (std::is_same_v<T, gpu::IndexBuf *>) {
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

  /* --- Bounds async readback (1-frame delayed) --- */
  if (bounds_enabled && bounds_ssbo) {
    /* Read previous frame's bounds (non-blocking if data ready). */
    uint32_t bounds_u[8];
    bool has_bounds = GPU_storagebuf_read_fast(bounds_ssbo, bounds_u);

    /* Re-initialize bounds SSBO to +INF/-INF for the NEXT frame's dispatch. */
    auto ordered_from_bits = [](uint32_t u) -> uint32_t {
      return (u & 0x80000000u) ? ~u : (u ^ 0x80000000u);
    };
    uint32_t init_data[8];
    const uint32_t ord_pos_inf = ordered_from_bits(0x7F800000u); /* +INF */
    const uint32_t ord_neg_inf = ordered_from_bits(0xFF800000u); /* -INF */
    init_data[0] = ord_pos_inf; init_data[1] = ord_pos_inf;
    init_data[2] = ord_pos_inf; init_data[3] = ord_pos_inf;
    init_data[4] = ord_neg_inf; init_data[5] = ord_neg_inf;
    init_data[6] = ord_neg_inf; init_data[7] = ord_neg_inf;
    GPU_storagebuf_update(bounds_ssbo, init_data);

    /* Apply previous frame's bounds to mesh_eval if valid. */
    if (has_bounds) {
      auto ordered_to_bits = [](uint32_t ou) -> uint32_t {
        return (ou & 0x80000000u) ? (ou ^ 0x80000000u) : ~ou;
      };
      auto uint_to_float = [](uint32_t b) -> float {
        float f;
        memcpy(&f, &b, sizeof(f));
        return f;
      };
      float3 pmin(uint_to_float(ordered_to_bits(bounds_u[0])),
                  uint_to_float(ordered_to_bits(bounds_u[1])),
                  uint_to_float(ordered_to_bits(bounds_u[2])));
      float3 pmax(uint_to_float(ordered_to_bits(bounds_u[4])),
                  uint_to_float(ordered_to_bits(bounds_u[5])),
                  uint_to_float(ordered_to_bits(bounds_u[6])));

      if (std::isfinite(pmin.x) && std::isfinite(pmin.y) && std::isfinite(pmin.z) &&
          std::isfinite(pmax.x) && std::isfinite(pmax.y) && std::isfinite(pmax.z) &&
          !(pmin.x > pmax.x || pmin.y > pmax.y || pmin.z > pmax.z))
      {
        Bounds<float3> bounds(pmin, pmax);
        mesh_eval->runtime->bounds_cache.tag_dirty();
        mesh_eval->runtime->bounds_cache.ensure(
            [&bounds](Bounds<float3> &r_data) { r_data = bounds; });
      }
    }
  }

  return bke::GpuComputeStatus::Success;
}

bke::GpuComputeStatus BKE_mesh_gpu_scatter_to_corners(
    const Depsgraph *depsgraph,
    const Object *ob_eval,
    Span<bke::GpuMeshComputeBinding> caller_bindings,
    const std::function<void(gpu::shader::ShaderCreateInfo &)> &config_fn,
    const std::function<void(gpu::Shader *)> &post_bind_fn,
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

  /* Move the mesh data out of the cache while holding the mutex, then free
   * resources without the mutex to avoid deadlocks. */
  MeshGpuData data = std::move(it->second);
  MeshGPUCacheManager::get().mesh_cache().erase(it);
  lock.unlock();

  if (GPU_context_active_get()) {
    if (data.internal_resources) {
      mesh_gpu_free_internal_resources_ptr(data.internal_resources);
      data.internal_resources = nullptr;
    }
    BKE_mesh_gpu_topology_free(data.topology);
  }
  else {
    /* Defer freeing until a GPU context is available. Move the data to orphans. */
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

void BKE_mesh_gpu_internal_resources_free_for_mesh(Mesh *mesh_orig)
{
  if (!mesh_orig) {
    return;
  }
  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
  auto it = MeshGPUCacheManager::get().mesh_cache().find(mesh_orig);
  if (it == MeshGPUCacheManager::get().mesh_cache().end()) {
    return;
  }
  MeshGpuData &d = it->second;
  if (!d.internal_resources) {
    return;
  }
  if (GPU_context_active_get()) {
    /* Move internal resources out then free them without holding the mutex. */
    bke::MeshGpuInternalResources *ir = d.internal_resources;
    d.internal_resources = nullptr;
    lock.unlock();
    mesh_gpu_free_internal_resources_ptr(ir);
    lock.lock();
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

gpu::Shader *BKE_mesh_gpu_internal_shader_get(Mesh *mesh_orig, const std::string &key)
{
  if (!mesh_orig) {
    return nullptr;
  }
  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
  auto it = MeshGPUCacheManager::get().mesh_cache().find(mesh_orig);
  if (it == MeshGPUCacheManager::get().mesh_cache().end()) {
    return nullptr;
  }
  MeshGpuData &d = it->second;
  if (!d.internal_resources) {
    return nullptr;
  }
  auto *entry_ptr = d.internal_resources->shader_map.lookup_ptr(key);
  if (!entry_ptr) {
    return nullptr;
  }
  return entry_ptr->shader;
}

gpu::Shader *BKE_mesh_gpu_internal_shader_ensure(
    Mesh *mesh_orig,
    Object *ob_eval,
    const std::string &key,
    const gpu::shader::ShaderCreateInfo &info)
{
  if (!mesh_orig) {
    return nullptr;
  }

  MeshGpuData *d = BKE_mesh_gpu_ensure_data(mesh_orig, (Mesh *)ob_eval->data);
  if (!d) {
    return nullptr;
  }
  auto *entry_ptr = d->internal_resources->shader_map.lookup_ptr(key);
  if (entry_ptr) {
    return entry_ptr->shader;
  }
  /* Create shader (must be called with GL context active). */
  if (!GPU_context_active_get()) {
    return nullptr;
  }
  /* Cast is needed because ShaderCreateInfo is a C++ wrapper; assume compatible here. */
  gpu::Shader *sh = GPU_shader_create_from_info_python((GPUShaderCreateInfo *)&info,
                                                                false);
  if (!sh) {
    return nullptr;
  }
  d->internal_resources->shader_map.add_new(key, {sh});
  return sh;
}

gpu::StorageBuf *BKE_mesh_gpu_internal_ssbo_ensure(Mesh *mesh_orig,
                                                            Object *ob_eval,
                                                            const std::string &key,
                                                            size_t size)
{
  if (!mesh_orig) {
    return nullptr;
  }

  MeshGpuData *d = BKE_mesh_gpu_ensure_data(mesh_orig, (Mesh *)ob_eval->data);
  if (!d) {
    return nullptr;
  }
  auto *entry_ptr = d->internal_resources->ssbo_map.lookup_ptr(key);
  if (entry_ptr) {
    return entry_ptr->buffer;
  }
  if (!GPU_context_active_get()) {
    return nullptr;
  }
  gpu::StorageBuf *buf = GPU_storagebuf_create(size);
  if (!buf) {
    return nullptr;
  }
  GPU_storagebuf_clear_to_zero(buf);
  d->internal_resources->ssbo_map.add_new(key, {buf});
  return buf;
}

gpu::StorageBuf *BKE_mesh_gpu_internal_ssbo_get(Mesh *mesh_orig, const std::string &key)
{
  if (!mesh_orig) {
    return nullptr;
  }
  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
  auto it = MeshGPUCacheManager::get().mesh_cache().find(mesh_orig);
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

/* -------------------------------------------------------------------- */
/** \name UBO Cache Management (same pattern as SSBO)
 * \{ */

gpu::UniformBuf *BKE_mesh_gpu_internal_ubo_ensure(Mesh *mesh_orig,
                                                           Object *ob_eval,
                                                           const std::string &key,
                                                           size_t size)
{
  if (!mesh_orig) {
    return nullptr;
  }

  MeshGpuData *d = BKE_mesh_gpu_ensure_data(mesh_orig, (Mesh *)ob_eval->data);
  if (!d) {
    return nullptr;
  }
  auto *entry_ptr = d->internal_resources->ubo_map.lookup_ptr(key);
  if (entry_ptr) {
    return entry_ptr->buffer;
  }
  if (!GPU_context_active_get()) {
    return nullptr;
  }
  gpu::UniformBuf *buf = GPU_uniformbuf_create(size);
  if (!buf) {
    return nullptr;
  }
  d->internal_resources->ubo_map.add_new(key, {buf});
  return buf;
}

gpu::UniformBuf *BKE_mesh_gpu_internal_ubo_get(Mesh *mesh_orig, const std::string &key)
{
  if (!mesh_orig) {
    return nullptr;
  }
  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
  auto it = MeshGPUCacheManager::get().mesh_cache().find(mesh_orig);
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

/* IBO (Index Buffer) cache -------------------------------------------------*/
gpu::IndexBuf *BKE_mesh_gpu_internal_ibo_ensure(Mesh *mesh_orig,
                                                         Object *ob_eval,
                                                         const std::string &key,
                                                         size_t /*size*/)
{
  if (!mesh_orig) {
    return nullptr;
  }

  MeshGpuData *d = BKE_mesh_gpu_ensure_data(mesh_orig, (Mesh *)ob_eval->data);
  if (!d) {
    return nullptr;
  }
  auto *entry_ptr = d->internal_resources->ibo_map.lookup_ptr(key);
  if (entry_ptr) {
    return entry_ptr->buffer;
  }
  if (!GPU_context_active_get()) {
    return nullptr;
  }
  /* Create an empty index buffer on device (zero length). Callers create/upload actual
   * content via GPU_indexbuf APIs and can keep the pointer returned by this cache. */
  gpu::IndexBuf *ib = GPU_indexbuf_build_on_device(0);
  if (!ib) {
    return nullptr;
  }
  d->internal_resources->ibo_map.add_new(key, {ib});
  return ib;
}

gpu::IndexBuf *BKE_mesh_gpu_internal_ibo_get(Mesh *mesh_orig, const std::string &key)
{
  if (!mesh_orig) {
    return nullptr;
  }
  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
  auto it = MeshGPUCacheManager::get().mesh_cache().find(mesh_orig);
  if (it == MeshGPUCacheManager::get().mesh_cache().end()) {
    return nullptr;
  }
  MeshGpuData &d = it->second;
  if (!d.internal_resources) {
    return nullptr;
  }
  auto *entry_ptr = d.internal_resources->ibo_map.lookup_ptr(key);
  if (!entry_ptr) {
    return nullptr;
  }
  return entry_ptr->buffer;
}

/* VBO (Vertex Buffer) cache -------------------------------------------------*/
gpu::VertBuf *BKE_mesh_gpu_internal_vbo_ensure(Mesh *mesh_orig,
                                                        Object *ob_eval,
                                                        const std::string &key,
                                                        size_t /*size*/)
{
  if (!mesh_orig) {
    return nullptr;
  }

  MeshGpuData *d = BKE_mesh_gpu_ensure_data(mesh_orig, (Mesh *)ob_eval->data);
  if (!d) {
    return nullptr;
  }
  auto *entry_ptr = d->internal_resources->vbo_map.lookup_ptr(key);
  if (entry_ptr) {
    return entry_ptr->buffer;
  }
  if (!GPU_context_active_get()) {
    return nullptr;
  }
  /* Create an empty vertex buffer. Callers should initialize format/size and upload data. */
  gpu::VertBuf *vb = GPU_vertbuf_calloc();
  if (!vb) {
    return nullptr;
  }
  d->internal_resources->vbo_map.add_new(key, {vb});
  return vb;
}

gpu::VertBuf *BKE_mesh_gpu_internal_vbo_get(Mesh *mesh_orig, const std::string &key)
{
  if (!mesh_orig) {
    return nullptr;
  }
  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
  auto it = MeshGPUCacheManager::get().mesh_cache().find(mesh_orig);
  if (it == MeshGPUCacheManager::get().mesh_cache().end()) {
    return nullptr;
  }
  MeshGpuData &d = it->second;
  if (!d.internal_resources) {
    return nullptr;
  }
  auto *entry_ptr = d.internal_resources->vbo_map.lookup_ptr(key);
  if (!entry_ptr) {
    return nullptr;
  }
  return entry_ptr->buffer;
}

/* Texture cache -------------------------------------------------*/
gpu::Texture *BKE_mesh_gpu_internal_texture_ensure(Mesh *mesh_orig,
                                                            Object *ob_eval,
                                                            const std::string &key,
                                                            gpu::Texture *texture)
{
  if (!mesh_orig || !texture) {
    return nullptr;
  }

  MeshGpuData *d = BKE_mesh_gpu_ensure_data(mesh_orig, (Mesh *)ob_eval->data);
  if (!d) {
    return nullptr;
  }

  /* Check if texture already exists */
  auto *entry_ptr = d->internal_resources->texture_map.lookup_ptr(key);
  if (entry_ptr) {
    /* Texture already exists: free old one if different */
    if (entry_ptr->texture != texture) {
      if (GPU_context_active_get()) {
        GPU_texture_free(entry_ptr->texture);
      }
      entry_ptr->texture = texture;
    }
    return texture;
  }

  /* Add new texture to cache */
  if (GPU_context_active_get()) {
    d->internal_resources->texture_map.add_new(key, {texture});
    return texture;
  }

  return nullptr;
}

gpu::Texture *BKE_mesh_gpu_internal_texture_get(Mesh *mesh_orig, const std::string &key)
{
  if (!mesh_orig) {
    return nullptr;
  }
  std::unique_lock<std::mutex> lock(MeshGPUCacheManager::get().mutex());
  auto it = MeshGPUCacheManager::get().mesh_cache().find(mesh_orig);
  if (it == MeshGPUCacheManager::get().mesh_cache().end()) {
    return nullptr;
  }
  MeshGpuData &d = it->second;
  if (!d.internal_resources) {
    return nullptr;
  }
  auto *entry_ptr = d.internal_resources->texture_map.lookup_ptr(key);
  if (!entry_ptr) {
    return nullptr;
  }
  return entry_ptr->texture;
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
      /* Move entries out of the cache, then free resources outside the mutex. */
      Vector<MeshGpuData> entries;
      entries.reserve(MeshGPUCacheManager::get().mesh_cache().size());
      for (auto &kv : MeshGPUCacheManager::get().mesh_cache()) {
        entries.append(std::move(kv.second));
      }
      MeshGPUCacheManager::get().mesh_cache().clear();
      lock.unlock();
      for (auto &d : entries) {
        if (d.internal_resources) {
          mesh_gpu_free_internal_resources_ptr(d.internal_resources);
          d.internal_resources = nullptr;
        }
        BKE_mesh_gpu_topology_free(d.topology);
      }
      lock.lock();
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

}  // namespace bke
}  // namespace blender
