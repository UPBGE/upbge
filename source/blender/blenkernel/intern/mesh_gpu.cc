/* SPDX-License-Identifier: GPL-2.0-or-later */

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

struct MeshGpuData {
  blender::bke::MeshGPUTopology topology;
  /* Support multiple compute shaders per mesh keyed by hash of generated source. */
  std::unordered_map<size_t, blender::gpu::Shader *> compute_shaders;
};

static std::unordered_map<const Mesh *, MeshGpuData> g_mesh_data_cache;
static std::vector<MeshGpuData> g_mesh_data_orphans;
static std::mutex g_mesh_cache_mutex;

static void mesh_gpu_orphans_flush()
{
  std::lock_guard<std::mutex> lock(g_mesh_cache_mutex);

  if (!GPU_context_active_get()) {
    return;
  }

  for (MeshGpuData &d : g_mesh_data_orphans) {
    for (auto &pair : d.compute_shaders) {
      if (pair.second) {
        GPU_shader_free(pair.second);
        pair.second = nullptr;
      }
    }
    d.compute_shaders.clear();
    BKE_mesh_gpu_topology_free(d.topology);
  }
  g_mesh_data_orphans.clear();
}

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
    mesh_gpu_orphans_flush();
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
  if (format->stride != 16) {
    BKE_mesh_gpu_free_for_mesh(mesh_orig);
    if (mesh_orig) {
      mesh_orig->is_using_gpu_deform = 1;
    }
    DEG_id_tag_update(&ob_orig->id, ID_RECALC_GEOMETRY);
    WM_main_add_notifier(NC_WINDOW, nullptr);
    return blender::bke::GpuComputeStatus::NotReady;
  }
  else {
    // reset is_using_gpu_deform as soon as possible
    mesh_orig->is_using_gpu_deform = 0;
  }

  if (format->stride == 16 && (ob_orig->id.recalc & ID_RECALC_GEOMETRY) != 0) {
    BKE_mesh_gpu_free_for_mesh(mesh_orig);
    return blender::bke::GpuComputeStatus::NotReady;
  }

  mesh_eval->is_running_gpu_deform = 1;

  std::lock_guard<std::mutex> lock(g_mesh_cache_mutex);

  auto &mesh_data = g_mesh_data_cache[mesh_orig];
  /* Create/upload topology (from evaluated mesh) if needed. On failure cleanup. */
  if (!mesh_data.topology.ssbo) {
    if (!BKE_mesh_gpu_topology_create(mesh_eval, mesh_data.topology) ||
        !BKE_mesh_gpu_topology_upload(mesh_data.topology))
    {
      BKE_mesh_gpu_free_for_mesh(mesh_orig);
      mesh_eval->is_running_gpu_deform = 0;
      return blender::bke::GpuComputeStatus::Error;
    }
  }
  std::string glsl_accessors = BKE_mesh_gpu_topology_glsl_accessors_string(mesh_data.topology);
  const std::string shader_source = glsl_accessors + main_glsl;
  /* Build shader identifier only from user glsl sources (not 100% fiable) */
  std::string shader_key_src = shader_source;
  const size_t shader_key = std::hash<std::string>()(shader_key_src);

  /* Lookup existing shader for this mesh + variant. */
  blender::gpu::Shader *shader = nullptr;
  auto it_shader = mesh_data.compute_shaders.find(shader_key);
  if (it_shader != mesh_data.compute_shaders.end()) {
    shader = it_shader->second;
  }
  else {
    using namespace blender::gpu::shader;
    ShaderCreateInfo info("compute_shader");
    info.local_group_size(256, 1, 1);
    info.compute_source("draw_colormanagement_lib.glsl");
    info.compute_source_generated = shader_source;

    /* User buffer bindings */
    for (const auto &binding : caller_bindings) {
      info.storage_buf(
          binding.binding, binding.qualifiers, binding.type_name, binding.bind_name);
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

    shader = GPU_shader_create_from_info((GPUShaderCreateInfo *)&info);
    if (!shader) {
      return blender::bke::GpuComputeStatus::Error;
    }
    mesh_data.compute_shaders.emplace(shader_key, shader);
  }

  /* Bind shader, bind buffers, update uniforms, and compute */
  const blender::gpu::shader::SpecializationConstants *constants =
      &GPU_shader_get_default_constant_state(shader);
  GPU_shader_bind(shader, constants);

  for (const auto &binding : caller_bindings) {
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
  const Depsgraph* depsgraph,
  const Object* ob_eval,
  blender::Span<blender::bke::GpuMeshComputeBinding> caller_bindings,
  const std::function<void(blender::gpu::shader::ShaderCreateInfo&)>& config_fn,
  const std::function<void(blender::gpu::Shader*)>& post_bind_fn,
  int dispatch_count)
{
  return BKE_mesh_gpu_run_compute(
      depsgraph,
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

  std::lock_guard<std::mutex> lock(g_mesh_cache_mutex);
  auto it = g_mesh_data_cache.find(mesh);
  if (it == g_mesh_data_cache.end()) {
    /* Ensure flag reset even if no cached data */
    mesh->is_using_gpu_deform = 0;
    return;
  }

  /* Move data out of the cache map. */
  MeshGpuData data = std::move(it->second);
  g_mesh_data_cache.erase(it);

  if (GPU_context_active_get()) {
    /* Immediate GPU-safe deletion. */
    for (auto &pair : data.compute_shaders) {
      if (pair.second) {
        GPU_shader_free(pair.second);
        pair.second = nullptr;
      }
    }
    data.compute_shaders.clear();
    BKE_mesh_gpu_topology_free(data.topology);
  }
  else {
    /* Defer freeing until a GPU context is available. */
    g_mesh_data_orphans.push_back(std::move(data));
  }

  mesh->is_using_gpu_deform = 0;
}

void BKE_mesh_gpu_free_all_caches()
{
  std::lock_guard<std::mutex> lock(g_mesh_cache_mutex);

  /* Free per-mesh cache entries. */
  for (auto &pair : g_mesh_data_cache) {
    MeshGpuData &data = pair.second;
    for (auto &sp : data.compute_shaders) {
      if (sp.second && GPU_context_active_get()) {
        GPU_shader_free(sp.second);
        sp.second = nullptr;
      }
    }
    BKE_mesh_gpu_topology_free(data.topology);
  }
  g_mesh_data_cache.clear();

  /* Try to flush any deferred frees if we have a context. */
  if (GPU_context_active_get()) {
    mesh_gpu_orphans_flush();
  }
}
