/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "render_graph/nodes/vk_pipeline_data.hh"
#include "render_graph/vk_command_buffer_wrapper.hh"
#include "render_graph/vk_render_graph_links.hh"

namespace blender::gpu::render_graph {
void vk_pipeline_data_copy(VKPipelineData &dst, const VKPipelineData &src)
{
  dst.push_constants_data = nullptr;
  dst.push_constants_size = src.push_constants_size;
  if (src.push_constants_size) {
    BLI_assert(src.push_constants_data);
    void *data = MEM_mallocN(src.push_constants_size, __func__);
    memcpy(data, src.push_constants_data, src.push_constants_size);
    dst.push_constants_data = data;
  }
}

void vk_pipeline_viewport_set_commands(VKCommandBufferInterface &command_buffer,
                                       const VKViewportData &viewport_data,
                                       VKViewportData &r_viewport_state)
{
  if (assign_if_different(r_viewport_state, viewport_data)) {
    command_buffer.set_viewport(viewport_data.viewports);
    command_buffer.set_scissor(viewport_data.scissors);
  }
}

void vk_pipeline_data_build_commands(VKCommandBufferInterface &command_buffer,
                                     const VKPipelineData &pipeline_data,
                                     VKBoundPipeline &r_bound_pipeline,
                                     VkPipelineBindPoint vk_pipeline_bind_point,
                                     VkShaderStageFlags vk_shader_stage_flags)
{
  if (assign_if_different(r_bound_pipeline.vk_pipeline, pipeline_data.vk_pipeline)) {
    command_buffer.bind_pipeline(vk_pipeline_bind_point, r_bound_pipeline.vk_pipeline);
  }

  if (assign_if_different(r_bound_pipeline.vk_descriptor_set, pipeline_data.vk_descriptor_set) &&
      r_bound_pipeline.vk_descriptor_set != VK_NULL_HANDLE)
  {
    command_buffer.bind_descriptor_sets(vk_pipeline_bind_point,
                                        pipeline_data.vk_pipeline_layout,
                                        0,
                                        1,
                                        &r_bound_pipeline.vk_descriptor_set,
                                        0,
                                        nullptr);
  }

  if (assign_if_different(r_bound_pipeline.descriptor_buffer_device_address,
                          pipeline_data.descriptor_buffer_device_address) &&
      r_bound_pipeline.descriptor_buffer_device_address != 0)
  {
    r_bound_pipeline.descriptor_buffer_offset = pipeline_data.descriptor_buffer_offset;
    VkDescriptorBufferBindingInfoEXT descriptor_buffer_binding_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
        nullptr,
        r_bound_pipeline.descriptor_buffer_device_address,
        VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT};
    command_buffer.bind_descriptor_buffers(1, &descriptor_buffer_binding_info);

    uint32_t buffer_index = 0;
    command_buffer.set_descriptor_buffer_offsets(vk_pipeline_bind_point,
                                                 pipeline_data.vk_pipeline_layout,
                                                 0,
                                                 1,
                                                 &buffer_index,
                                                 &r_bound_pipeline.descriptor_buffer_offset);
  }
  else if (assign_if_different(r_bound_pipeline.descriptor_buffer_offset,
                               pipeline_data.descriptor_buffer_offset) &&
           r_bound_pipeline.descriptor_buffer_device_address != 0)
  {
    uint32_t buffer_index = 0;
    command_buffer.set_descriptor_buffer_offsets(vk_pipeline_bind_point,
                                                 pipeline_data.vk_pipeline_layout,
                                                 0,
                                                 1,
                                                 &buffer_index,
                                                 &r_bound_pipeline.descriptor_buffer_offset);
  }

  if (pipeline_data.push_constants_size) {
    command_buffer.push_constants(pipeline_data.vk_pipeline_layout,
                                  vk_shader_stage_flags,
                                  0,
                                  pipeline_data.push_constants_size,
                                  pipeline_data.push_constants_data);
  }
}

void vk_pipeline_data_free(VKPipelineData &data)
{
  if (data.push_constants_data) {
    MEM_freeN(const_cast<void *>(data.push_constants_data));
    data.push_constants_data = nullptr;
  }
}

void vk_index_buffer_binding_build_links(VKResourceStateTracker &resources,
                                         VKRenderGraphNodeLinks &node_links,
                                         const VKIndexBufferBinding &index_buffer_binding)
{
  ResourceWithStamp resource = resources.get_buffer(index_buffer_binding.buffer);
  node_links.inputs.append({resource, VK_ACCESS_INDEX_READ_BIT});
}

void vk_index_buffer_binding_build_commands(VKCommandBufferInterface &command_buffer,
                                            const VKIndexBufferBinding &index_buffer_binding,
                                            VKIndexBufferBinding &r_bound_index_buffer)
{
  if (assign_if_different(r_bound_index_buffer, index_buffer_binding)) {
    command_buffer.bind_index_buffer(
        r_bound_index_buffer.buffer, 0, r_bound_index_buffer.index_type);
  }
}

void vk_vertex_buffer_bindings_build_links(VKResourceStateTracker &resources,
                                           VKRenderGraphNodeLinks &node_links,
                                           const VKVertexBufferBindings &vertex_buffers)
{
  node_links.inputs.reserve(node_links.inputs.size() + vertex_buffers.buffer_count);
  for (const VkBuffer vk_buffer :
       Span<VkBuffer>(vertex_buffers.buffer, vertex_buffers.buffer_count))
  {
    ResourceWithStamp resource = resources.get_buffer(vk_buffer);
    node_links.inputs.append({resource, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT});
  }
}

void vk_vertex_buffer_bindings_build_commands(VKCommandBufferInterface &command_buffer,
                                              const VKVertexBufferBindings &vertex_buffer_bindings,
                                              VKVertexBufferBindings &r_bound_vertex_buffers)
{
  if (assign_if_different(r_bound_vertex_buffers, vertex_buffer_bindings) &&
      r_bound_vertex_buffers.buffer_count)
  {
    command_buffer.bind_vertex_buffers(0,
                                       r_bound_vertex_buffers.buffer_count,
                                       r_bound_vertex_buffers.buffer,
                                       r_bound_vertex_buffers.offset);
  }
}

}  // namespace blender::gpu::render_graph
