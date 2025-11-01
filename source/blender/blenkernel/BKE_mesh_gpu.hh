/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <functional>
#include <variant>

#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "GPU_shader.hh"
#include "GPU_index_buffer.hh"
#include "GPU_storage_buffer.hh"
#include "GPU_uniform_buffer.hh"
#include "GPU_vertex_buffer.hh"

#include "../gpu/intern/gpu_shader_create_info.hh"

struct Mesh;
struct Object;
struct Depsgraph;

namespace blender::bke {

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

}  // namespace blender::bke

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
bool BKE_mesh_gpu_topology_create(const Mesh *mesh, blender::bke::MeshGPUTopology &topology);

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

/**
 * Free all cached GPU resources associated with a specific mesh.
 * This should be called when a mesh is modified or freed to prevent memory leaks.
 *
 * \param mesh: The mesh for which to free cached resources.
 */
void BKE_mesh_gpu_free_for_mesh(Mesh *mesh);

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
