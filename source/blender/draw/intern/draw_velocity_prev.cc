/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/** \file
 * \ingroup eevee (UPBGE)
 */

#include "draw_velocity_prev.hh"

#include "GPU_compute.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_vertex_buffer.hh"

#include "../gpu/intern/gpu_shader_create_info.hh"

namespace blender::draw {

/* Warning: KEY on original Object (ob_orig) — Mesh* can change between evaluations. */
static Map<Object *, gpu::VertBuf *> prev_vbo_map;
static blender::gpu::Shader *copy_shader = nullptr;
/* Reference count for shader users. */
static int copy_shader_refcount = 0;

/* Create the compute shader used to copy vec4 elements from src -> dst. */
static void ensure_copy_shader()
{
  if (copy_shader) {
    return;
  }

  using namespace blender::gpu::shader;
  ShaderCreateInfo info("DRAW_Copy_VertBuf");
  const int group_size = 256;
  info.local_group_size(group_size, 1, 1);
  info.compute_source("draw_colormanagement_lib.glsl");
  info.storage_buf(0, Qualifier::write, "vec4", "out_buf[]");
  info.storage_buf(1, Qualifier::read, "vec4", "in_buf[]");

  info.compute_source_generated = R"GLSL(
void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= in_buf.length()) {
    return;
  }
  out_buf[i] = in_buf[i];
}
)GLSL";

  copy_shader = GPU_shader_create_from_info((GPUShaderCreateInfo *)&info);
}

/* Acquire / release helpers for shader lifetime.
 * The shader is created on the first acquire and freed on the last release. */
static void acquire_copy_shader()
{
  if (++copy_shader_refcount == 1) {
    ensure_copy_shader();
  }
}

static void release_copy_shader()
{
  if (copy_shader_refcount <= 0) {
    return;
  }
  copy_shader_refcount--;
  if (copy_shader_refcount == 0) {
    if (copy_shader) {
      GPU_shader_free(copy_shader);
      copy_shader = nullptr;
    }
  }
}

/* Ensure prev VBO exists. Keyed by original Object pointer. */
gpu::VertBuf *ensure_prev_pos_vbo(Object *ob, int verts_num, const GPUVertFormat *format)
{
  if (!ob) {
    return nullptr;
  }

  gpu::VertBuf *&vb = prev_vbo_map.lookup_or_add_default(ob);
  if (!vb) {
    /* Create a new vertbuf with same format and allocate vertices.
     * The shader is needed when a prev_vbo exists (to copy into it). */
    vb = GPU_vertbuf_create_with_format(*format);
    GPU_vertbuf_data_alloc(*vb, verts_num);
    acquire_copy_shader();
  }
  return vb;
}

gpu::VertBuf *get_prev_pos_vbo(Object *ob)
{
  if (!ob) {
    return nullptr;
  }
  return prev_vbo_map.lookup_default(ob, nullptr);
}

void free_prev_pos_vbo(Object *ob)
{
  if (!ob) {
    return;
  }
  gpu::VertBuf *vb = prev_vbo_map.lookup_default(ob, nullptr);
  if (!vb) {
    return;
  }
  GPU_vertbuf_discard(vb);
  prev_vbo_map.remove(ob);
  /* Release shader ref held for this prev_vbo. */
  release_copy_shader();
}

/* Optional: free all prev vbos and shader (call at shutdown). */
void prev_vbo_shutdown()
{
  for (MapItem<Object *, gpu::VertBuf *> item : prev_vbo_map.items()) {
    gpu::VertBuf *vb = item.value;
    if (vb) {
      GPU_vertbuf_discard(vb);
    }
  }
  prev_vbo_map.clear();
  /* reset refcount and free shader if any */
  copy_shader_refcount = 0;
  if (copy_shader) {
    GPU_shader_free(copy_shader);
    copy_shader = nullptr;
  }
}

/* Copy src vertbuf -> dst vertbuf on GPU using a compute shader.
 * Both buffers must contain float4 positions (or at least be compatible for vec4 copy).
 * verts is the vertex count to copy. */
void copy_vertbuf_to_vertbuf(gpu::VertBuf *dst, gpu::VertBuf *src, int verts)
{
  if (!dst || !src || verts <= 0) {
    return;
  }

  /* Quick no-op if same buffer. */
  if (dst == src) {
    return;
  }

  /* Ensure shader exists. Note: ensures refcount is non-zero only if
   * someone called ensure_prev_pos_vbo earlier - this call doesn't modify refcount. */
  ensure_copy_shader();

  /* Bind dst and src as SSBOs for the compute shader.
   * Note: GPU API in this tree allows binding a GPUVertBuf as SSBO via
   * gpu::VertBuf::bind_as_ssbo. */
  dst->bind_as_ssbo(0);
  src->bind_as_ssbo(1);

  GPU_shader_bind(copy_shader);

  const int group_size = 256;
  const int groups = (verts + group_size - 1) / group_size;
  GPU_compute_dispatch(copy_shader, groups, 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_VERTEX_ATTRIB_ARRAY);

  GPU_shader_unbind();
}

}  // namespace blender::draw
