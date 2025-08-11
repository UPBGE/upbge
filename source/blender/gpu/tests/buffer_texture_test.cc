/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "GPU_capabilities.hh"
#include "GPU_compute.hh"
#include "GPU_state.hh"
#include "GPU_vertex_buffer.hh"
#include "GPU_vertex_format.hh"

#include "BLI_math_vector_types.hh"

#include "gpu_testing.hh"

namespace blender::gpu::tests {

static void test_buffer_texture()
{
  /* Build compute shader. */
  gpu::Shader *shader = GPU_shader_create_from_info_name("gpu_buffer_texture_test");
  EXPECT_NE(shader, nullptr);
  GPU_shader_bind(shader);

  /* Vertex buffer. */
  GPUVertFormat format = {};
  uint value_pos = GPU_vertformat_attr_add(&format, "value", gpu::VertAttrType::SFLOAT_32);
  VertBuf *vertex_buffer = GPU_vertbuf_create_with_format_ex(format,
                                                             GPU_USAGE_FLAG_BUFFER_TEXTURE_ONLY);
  float4 value = float4(42.42, 23.23, 1.0, -1.0);
  GPU_vertbuf_data_alloc(*vertex_buffer, 4);
  GPU_vertbuf_attr_fill(vertex_buffer, value_pos, &value);
  GPU_vertbuf_bind_as_texture(vertex_buffer,
                              GPU_shader_get_sampler_binding(shader, "bufferTexture"));

  /* Construct SSBO. */
  StorageBuf *ssbo = GPU_storagebuf_create_ex(
      4 * sizeof(float), nullptr, GPU_USAGE_STATIC, __func__);
  GPU_storagebuf_bind(ssbo, GPU_shader_get_ssbo_binding(shader, "data_out"));

  /* Dispatch compute task. */
  GPU_compute_dispatch(shader, 4, 1, 1);

  /* Check if compute has been done. */
  GPU_memory_barrier(GPU_BARRIER_BUFFER_UPDATE);

  /* Download the storage buffer. */
  float4 read_data;
  GPU_storagebuf_read(ssbo, read_data);
  EXPECT_EQ(read_data, value);

  /* Cleanup. */
  GPU_shader_unbind();
  GPU_storagebuf_free(ssbo);
  GPU_vertbuf_discard(vertex_buffer);
  GPU_shader_free(shader);
}

GPU_TEST(buffer_texture)

}  // namespace blender::gpu::tests
