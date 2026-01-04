/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <functional>
#include <variant>

#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "GPU_index_buffer.hh"
#include "GPU_shader.hh"
#include "GPU_storage_buffer.hh"
#include "GPU_uniform_buffer.hh"
#include "GPU_vertex_buffer.hh"

#include "../gpu/intern/gpu_shader_create_info.hh"

struct Mesh;
struct Object;
struct Depsgraph;

namespace blender {
namespace bke {

/**
 * Mesh GPU topology data for compute shaders.
 * Contains packed mesh topology data with computed offsets for efficient GPU access.
 */
struct MeshGPUTopology {
  /* Packed topology data arrays with their offsets */
  int face_offsets_offset = 0;
  int corner_to_face_offset = 0;
  int corner_verts_offset = 0;
  int corner_tris_offset = 0;
  int corner_tri_faces_offset = 0;
  int edges_offset = 0;
  int corner_edges_offset = 0;
  int vert_to_face_offsets_offset = 0;
  int vert_to_face_offset = 0;

  /* Total size of packed data */
  int total_size = 0;

  /* Packed data vector */
  blender::Vector<int> data;

  /* GPU storage buffer (null if not uploaded) */
  blender::gpu::StorageBuf *ssbo = nullptr;

  /* Constructor */
  MeshGPUTopology() = default;
};

/**
 * Describes a buffer binding for a mesh compute operation.
 */
struct GpuMeshComputeBinding {
  /** The binding point index in the shader (layout(binding = ...)). */
  int binding;
  /** The buffer to bind. Can be a StorageBuf* or a VertBuf* or a UniformBuf* or a IndexBuf*. */
  std::variant<blender::gpu::StorageBuf *,
               blender::gpu::VertBuf *,
               blender::gpu::UniformBuf *,
               blender::gpu::IndexBuf *>
      buffer;
  /** Qualifiers (read, write, read_write). */
  blender::gpu::shader::Qualifier qualifiers;
  /** GLSL type name for the buffer declaration (e.g., "vec4", "uint"). */
  const char *type_name;
  /** GLSL variable name for the buffer declaration (e.g., "my_output_buffer[]"). */
  const char *bind_name;
};

enum class GpuComputeStatus {
  Success,
  NotReady,
  Error,
};

}  // namespace bke
}  // namespace blender

// Type alias for the old MeshGPUTopology type for compatibility
using MeshGPUTopology = blender::bke::MeshGPUTopology;

/**
 * Build mesh topology data for GPU compute shaders.
 * Packs face offsets, corner-to-face mapping, corner vertices, corner triangles,
 * triangle-to-face mapping, edges, corner edges, vertex-to-face offsets and indices into a single
 * buffer.
 *
 * \param mesh: Source mesh data
 * \param topology: Output topology structure with computed offsets and data
 * \return true on success, false on failure
 */
bool BKE_mesh_gpu_topology_create(const Mesh *mesh_eval, blender::bke::MeshGPUTopology &topology);

/**
 * Upload mesh topology data to GPU storage buffer.
 * Creates or updates the SSBO with the packed topology data.
 *
 * \param topology: Topology data to upload
 * \return true on success, false on failure (e.g., no GPU context)
 */
bool BKE_mesh_gpu_topology_upload(blender::bke::MeshGPUTopology &topology);

/**
 * Free GPU resources associated with topology data.
 * Safe to call multiple times or without GPU context.
 *
 * \param topology: Topology data to free
 */
void BKE_mesh_gpu_topology_free(blender::bke::MeshGPUTopology &topology);

/**
 * A high-level utility to run a compute shader on a mesh.
 *
 * This function automates:
 * - Creation and caching of mesh topology and position SSBOs.
 * - Dynamic shader creation by combining generated helpers with user-provided code.
 * - Binding of all necessary buffers.
 * - Dispatching the compute shader.
 *
 * \param mesh: The mesh to operate on.
 * \param main_glsl: The GLSL source code for the `main()` function of the compute shader.
 * \param bindings: A span of additional SSBOs to bind.
 * \param dispatch_count: The number of elements to process (e.g., mesh->verts_num).
 * \return A status indicating success, failure, or if resources are not ready.
 */
blender::bke::GpuComputeStatus BKE_mesh_gpu_run_compute(
    const Depsgraph *depsgraph,
    const Object *ob_eval,
    const char *main_glsl,
    blender::Span<blender::bke::GpuMeshComputeBinding> caller_bindings,
    const std::function<void(blender::gpu::shader::ShaderCreateInfo &)> &config_fn,
    const std::function<void(blender::gpu::Shader *)> &post_bind_fn = {},
    int dispatch_count = 0);

blender::bke::GpuComputeStatus BKE_mesh_gpu_scatter_to_corners(
    const Depsgraph *depsgraph,
    const Object *ob_eval,
    blender::Span<blender::bke::GpuMeshComputeBinding> caller_bindings,
    const std::function<void(blender::gpu::shader::ShaderCreateInfo &)> &config_fn,
    const std::function<void(blender::gpu::Shader *)> &post_bind_fn,
    int dispatch_count);

/**
 * Free all cached GPU resources associated with a specific mesh.
 * This should be called when a mesh is modified or freed to prevent memory leaks.
 *
 * \param mesh: The mesh for which to free cached resources.
 */
void BKE_mesh_gpu_free_for_mesh(Mesh *mesh_orig);

/**
 * Request a GPU geometry recalc for the given mesh.
 *
 * Sets flags to:
 * - Skip CPU modifier stack evaluation (is_running_gpu_skinning = 1)
 * - Preserve mesh_eval (no free) - Prevents BKE_mesh_batch_cache_dirty_tag
 * (is_running_gpu_skinning = 1)
 *
 * Triggers:
 * - Depsgraph geometry tag
 * - Viewport redraw notification to reconstruct render cache with right vbos format
 *
 * \param mesh_orig: Original mesh
 * \param mesh_eval: Evaluated mesh
 * \param ob_orig: Original object
 *
 * \note Designed to be called from BKE_mesh_gpu_run_compute when stride check fails.
 */
void BKE_mesh_request_gpu_render_cache_update(Mesh *mesh_orig, Mesh *mesh_eval, Object *ob_orig);

/**
 * Cleanup function to be called on Blender exit to free all cached compute resources,
 * including all compiled shaders and mesh data.
 */
void BKE_mesh_gpu_free_all_caches();

/**
 * Get accessor functions for GLSL shader integration.
 * Returns strings containing GLSL functions to access topology data by offset.
 *
 * \param topology: Topology data with computed offsets
 * \return GLSL accessor functions as string
 */
std::string BKE_mesh_gpu_topology_glsl_accessors_string(
    const blender::bke::MeshGPUTopology &topology);

/**
 * Add all topology offsets from a MeshGPUTopology struct as specialization
 * constants to a shader create info object.
 *
 * This automates the process of keeping the shader constants in sync with the
 * struct definition.
 *
 * \param info: The shader create info to add constants to.
 * \param topology: The topology data containing the offsets.
 */
void BKE_mesh_gpu_topology_add_specialization_constants(
    blender::gpu::shader::ShaderCreateInfo &info, const blender::bke::MeshGPUTopology &topology);

/* Forward declaration to avoid including mesh_gpu_cache.hh here. */
namespace blender {
namespace bke {
struct MeshGpuData;
}
}

/* Ensure mesh GPU data exists: topology SSBO (from evaluated mesh) and internal resources.
 * Returns pointer to MeshGpuData on success, nullptr on failure. */
blender::bke::MeshGpuData *BKE_mesh_gpu_ensure_data(struct Mesh *mesh_orig,
                                                    struct Mesh *mesh_eval);

namespace blender {
namespace bke {

/**
 * Internal GPU resources owned by BKE for a mesh. These are meant for internal usage only
 * (not exposed to Python) and are freed when the mesh batch cache is freed or on invalidation.
 */
struct MeshGpuInternalResources {
  /* Entry for cached SSBO */
  struct SsboEntry {
    blender::gpu::StorageBuf *buffer = nullptr;
  };

  /* Entry for cached UBO */
  struct UboEntry {
    blender::gpu::UniformBuf *buffer = nullptr;
  };

  /* Entry for cached shader */
  struct ShaderEntry {
    blender::gpu::Shader *shader = nullptr;
  };

  /* Entry for cached IBO */
  struct IboEntry {
    blender::gpu::IndexBuf *buffer = nullptr;
  };

  /* Entry for cached VBO */
  struct VboEntry {
    blender::gpu::VertBuf *buffer = nullptr;
  };

  /* Entry for cached Texture */
  struct TextureEntry {
    blender::gpu::Texture *texture = nullptr;
  };

  /* Keyed maps to prevent duplicate resources */
  blender::Map<std::string, SsboEntry> ssbo_map;
  blender::Map<std::string, UboEntry> ubo_map;
  blender::Map<std::string, ShaderEntry> shader_map;
  blender::Map<std::string, IboEntry> ibo_map;
  blender::Map<std::string, VboEntry> vbo_map;
  blender::Map<std::string, TextureEntry> texture_map;

  MeshGpuInternalResources() = default;
};

}  // namespace bke
}  // namespace blender

/**
 * Free internal resources associated with a mesh. Safe to call multiple times.
 */
void BKE_mesh_gpu_internal_resources_free_for_mesh(Mesh *mesh_orig);

/* Helpers for Shaders */
blender::gpu::Shader *BKE_mesh_gpu_internal_shader_get(Mesh *mesh_orig, const std::string &key);
blender::gpu::Shader *BKE_mesh_gpu_internal_shader_ensure(
    Mesh *mesh_orig,
    Object *ob_eval,
    const std::string &key,
    const blender::gpu::shader::ShaderCreateInfo &info);


/* Helpers for storage buffers (SSBO) */
blender::gpu::StorageBuf *BKE_mesh_gpu_internal_ssbo_get(Mesh *mesh_orig, const std::string &key);
blender::gpu::StorageBuf *BKE_mesh_gpu_internal_ssbo_ensure(Mesh *mesh_orig,
                                                            Object *ob_eval,
                                                            const std::string &key,
                                                            size_t size);

/* Helpers for index buffers (IBO) */
blender::gpu::IndexBuf *BKE_mesh_gpu_internal_ibo_get(Mesh *mesh_orig, const std::string &key);
blender::gpu::IndexBuf *BKE_mesh_gpu_internal_ibo_ensure(Mesh *mesh_orig,
                                                         Object *ob_eval,
                                                         const std::string &key,
                                                         size_t size);

/* Helpers for vertex buffers (VBO) */
blender::gpu::VertBuf *BKE_mesh_gpu_internal_vbo_ensure(Mesh *mesh_orig,
                                                        Object *ob_eval,
                                                        const std::string &key,
                                                        size_t size);
blender::gpu::VertBuf *BKE_mesh_gpu_internal_vbo_get(Mesh *mesh_orig, const std::string &key);

/* Helpers for uniform buffers (UBO) */
blender::gpu::UniformBuf *BKE_mesh_gpu_internal_ubo_get(Mesh *mesh_orig, const std::string &key);
blender::gpu::UniformBuf *BKE_mesh_gpu_internal_ubo_ensure(Mesh *mesh_orig,
                                                           Object *ob_eval,
                                                           const std::string &key,
                                                           size_t size);

/* Helpers for textures (Texture) */
blender::gpu::Texture *BKE_mesh_gpu_internal_texture_get(Mesh *mesh_orig, const std::string &key);
blender::gpu::Texture *BKE_mesh_gpu_internal_texture_ensure(Mesh *mesh_orig,
                                                            Object *ob_eval,
                                                            const std::string &key,
                                                            blender::gpu::Texture *texture);
