/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Mimics old style opengl immediate mode drawing.
 */

#include "BKE_global.hh"

#include "GPU_vertex_format.hh"
#include "gpu_context_private.hh"
#include "gpu_shader_private.hh"
#include "gpu_vertex_format_private.hh"

#include "mtl_context.hh"
#include "mtl_debug.hh"
#include "mtl_immediate.hh"
#include "mtl_primitive.hh"
#include "mtl_shader.hh"

namespace blender::gpu {

MTLImmediate::MTLImmediate(MTLContext *ctx)
{
  context_ = ctx;
}

uchar *MTLImmediate::begin()
{
  BLI_assert(!has_begun_);

  /* Determine primitive type. */
  metal_primitive_type_ = gpu_prim_type_to_metal(this->prim_type);
  metal_primitive_mode_ = mtl_prim_type_to_topology_class(metal_primitive_type_);
  has_begun_ = true;

  /* If prim type is line loop, add an extra vertex at the end for placing the closing line,
   * as metal does not support this primitive type. We treat this as a Line strip with one
   * extra value. */
  int vertex_alloc_length = vertex_len;
  if (prim_type == GPU_PRIM_LINE_LOOP) {
    vertex_alloc_length++;
  }

  /* Allocate a range of data and return host-accessible pointer. */
  const size_t bytes_needed = vertex_buffer_size(&vertex_format, vertex_alloc_length);
  current_allocation_ = context_->get_scratchbuffer_manager()
                            .scratch_buffer_allocate_range_aligned(bytes_needed, 256);
  [current_allocation_.metal_buffer retain];
  return reinterpret_cast<uchar *>(current_allocation_.data);
}

void MTLImmediate::end()
{
  /* Ensure we're between a `imm::begin` / `imm:end` pair. */
  BLI_assert(has_begun_);
  BLI_assert(prim_type != GPU_PRIM_NONE);

  /* Verify context is valid, vertex data is written and a valid shader is bound. */
  if (context_ && this->vertex_idx > 0 && this->shader) {

    MTLShader *active_mtl_shader = static_cast<MTLShader *>(shader);

    /* Skip draw if Metal shader is not valid. */
    if (active_mtl_shader == nullptr || !active_mtl_shader->is_valid() ||
        active_mtl_shader->get_interface() == nullptr)
    {

      const StringRefNull ptr = (active_mtl_shader) ? active_mtl_shader->name_get() : "";
      MTL_LOG_WARNING(
          "MTLImmediate::end -- cannot perform draw as active shader is NULL or invalid (likely "
          "unimplemented) (shader %p '%s')",
          active_mtl_shader,
          ptr.c_str());
      return;
    }

    /* Ensure we are inside a render pass and fetch active RenderCommandEncoder. */
    id<MTLRenderCommandEncoder> rec = context_->ensure_begin_render_pass();
    BLI_assert(rec != nil);

    /* Fetch active render pipeline state. */
    MTLRenderPassState &rps = context_->main_command_buffer.get_render_pass_state();

    /* Bind Shader. */
    GPU_shader_bind(this->shader);

    /* Debug markers for frame-capture and detailed error messages. */
    if (G.debug & G_DEBUG_GPU) {
      [rec pushDebugGroup:[NSString
                              stringWithFormat:@"immEnd(verts: %d, shader: %s)",
                                               this->vertex_idx,
                                               active_mtl_shader->get_interface()->get_name()]];
      [rec insertDebugSignpost:[NSString stringWithFormat:@"immEnd(verts: %d, shader: %s)",
                                                          this->vertex_idx,
                                                          active_mtl_shader->get_interface()
                                                              ->get_name()]];
    }

    /* Populate pipeline state vertex descriptor. */
    MTLStateManager *state_manager = static_cast<MTLStateManager *>(
        MTLContext::get()->state_manager);
    MTLRenderPipelineStateDescriptor &desc = state_manager->get_pipeline_descriptor();
    const MTLShaderInterface *interface = active_mtl_shader->get_interface();

    /* Reset vertex descriptor to default state. */
    desc.reset_vertex_descriptor();
    desc.vertex_descriptor.total_attributes = interface->get_total_attributes();
    desc.vertex_descriptor.max_attribute_value = interface->get_total_attributes() - 1;
    desc.vertex_descriptor.num_vert_buffers = 1;

    for (int i = 0; i < desc.vertex_descriptor.total_attributes; i++) {
      desc.vertex_descriptor.attributes[i].format = MTLVertexFormatInvalid;
    }

    /* Populate Vertex descriptor and verify attributes.
     * TODO(Metal): Cache this vertex state based on Vertex format and shaders. */
    for (int i = 0; i < interface->get_total_attributes(); i++) {

      /* NOTE: Attribute in VERTEX FORMAT does not necessarily share the same array index as
       * attributes in shader interface. */
      GPUVertAttr *attr = nullptr;
      const MTLShaderInputAttribute &mtl_shader_attribute = interface->get_attribute(i);

      /* Scan through vertex_format attributes until one with a name matching the shader interface
       * is found. */
      for (uint32_t a_idx = 0; a_idx < this->vertex_format.attr_len && attr == nullptr; a_idx++) {
        GPUVertAttr *check_attribute = &this->vertex_format.attrs[a_idx];

        /* Attributes can have multiple name aliases associated with them. */
        for (uint32_t n_idx = 0; n_idx < check_attribute->name_len; n_idx++) {
          const char *name = GPU_vertformat_attr_name_get(
              &this->vertex_format, check_attribute, n_idx);

          if (strcmp(name, interface->get_name_at_offset(mtl_shader_attribute.name_offset)) == 0) {
            attr = check_attribute;
            break;
          }
        }
      }

      BLI_assert_msg(attr != nullptr,
                     "Could not find expected attribute in immediate mode vertex format.");
      if (attr == nullptr) {
        MTL_LOG_ERROR(
            "MTLImmediate::end Could not find matching attribute '%s' from Shader Interface in "
            "Vertex Format! - TODO: Bind Dummy attribute",
            interface->get_name_at_offset(mtl_shader_attribute.name_offset));
        return;
      }

      /* Determine whether implicit type conversion between input vertex format
       * and shader interface vertex format is supported. */
      MTLVertexFormat convertedFormat;
      bool can_use_implicit_conversion = mtl_convert_vertex_format(mtl_shader_attribute.format,
                                                                   attr->type.comp_type(),
                                                                   attr->type.comp_len(),
                                                                   attr->type.fetch_mode(),
                                                                   &convertedFormat);

      if (can_use_implicit_conversion) {
        /* Metal API can implicitly convert some formats during vertex assembly:
         * - Converting from a normalized short2 format to float2
         * - Type truncation e.g. Float4 to Float2.
         * - Type expansion from Float3 to Float4.
         * - NOTE: extra components are filled with the corresponding components of (0,0,0,1).
         * (See
         * https://developer.apple.com/documentation/metal/mtlvertexattributedescriptor/1516081-format)
         */
        bool is_floating_point_format = is_fetch_float(attr->type.format);
        desc.vertex_descriptor.attributes[i].format = convertedFormat;
        desc.vertex_descriptor.attributes[i].format_conversion_mode =
            (is_floating_point_format) ? (GPUVertFetchMode)GPU_FETCH_FLOAT :
                                         (GPUVertFetchMode)GPU_FETCH_INT;
        BLI_assert(convertedFormat != MTLVertexFormatInvalid);
      }
      else {
        /* Some conversions are NOT valid, e.g. Int4 to Float4
         * - In this case, we need to implement a conversion routine inside the shader.
         * - This is handled using the format_conversion_mode flag
         * - This flag is passed into the PSO as a function specialization,
         *   and will generate an appropriate conversion function when reading the vertex attribute
         *   value into local shader storage.
         *   (If no explicit conversion is needed, the function specialize to a pass-through). */
        MTLVertexFormat converted_format = format_resize_comp(mtl_shader_attribute.format,
                                                              attr->type.comp_len());
        desc.vertex_descriptor.attributes[i].format = converted_format;
        desc.vertex_descriptor.attributes[i].format_conversion_mode = attr->type.fetch_mode();
        BLI_assert(desc.vertex_descriptor.attributes[i].format != MTLVertexFormatInvalid);
      }
      /* Using attribute offset in vertex format, as this will be correct */
      desc.vertex_descriptor.attributes[i].offset = attr->offset;
      desc.vertex_descriptor.attributes[i].buffer_index = mtl_shader_attribute.buffer_index;
    }

    /* Buffer bindings for singular vertex buffer. */
    desc.vertex_descriptor.buffer_layouts[0].step_function = MTLVertexStepFunctionPerVertex;
    desc.vertex_descriptor.buffer_layouts[0].step_rate = 1;
    desc.vertex_descriptor.buffer_layouts[0].stride = this->vertex_format.stride;
    BLI_assert(this->vertex_format.stride > 0);

    /* Emulate LineLoop using LineStrip. */
    if (this->prim_type == GPU_PRIM_LINE_LOOP) {
      /* Patch final vertex of line loop to close. Rendered using LineStrip.
       * NOTE: vertex_len represents original length, however, allocated Metal
       * buffer contains space for one extra vertex when LineLoop is used. */
      uchar *buffer_data = reinterpret_cast<uchar *>(current_allocation_.data);
      memcpy(buffer_data + (vertex_len)*vertex_format.stride, buffer_data, vertex_format.stride);
      this->vertex_idx++;
      this->prim_type = GPU_PRIM_LINE_STRIP;
    }

    if (this->shader->is_polyline) {
      context_->get_scratchbuffer_manager().bind_as_ssbo(GPU_SSBO_POLYLINE_POS_BUF_SLOT);
      context_->get_scratchbuffer_manager().bind_as_ssbo(GPU_SSBO_POLYLINE_COL_BUF_SLOT);
      context_->get_scratchbuffer_manager().bind_as_ssbo(GPU_SSBO_INDEX_BUF_SLOT);
    }

    MTLPrimitiveType mtl_prim_type = gpu_prim_type_to_metal(this->prim_type);

    if (context_->ensure_render_pipeline_state(mtl_prim_type)) {

      /* Issue draw call. */
      BLI_assert(this->vertex_idx > 0);

      /* Metal API does not support triangle fan, so we can emulate this
       * input data by generating an index buffer to re-map indices to
       * a TriangleList.
       *
       * NOTE(Metal): Consider caching generated triangle fan index buffers.
       * For immediate mode, generating these is currently very cheap, as we use
       * fast scratch buffer allocations. Though we may benefit from caching of
       * frequently used buffer sizes. */
      bool rendered = false;
      if (mtl_needs_topology_emulation(this->prim_type)) {

        /* Emulate Tri-fan. */
        switch (this->prim_type) {
          case GPU_PRIM_TRI_FAN: {
            /* Prepare Triangle-Fan emulation index buffer on CPU based on number of input
             * vertices. */
            uint32_t base_vert_count = this->vertex_idx;
            uint32_t num_triangles = max_ii(base_vert_count - 2, 0);
            uint32_t fan_index_count = num_triangles * 3;
            BLI_assert(num_triangles > 0);

            uint32_t alloc_size = sizeof(uint32_t) * fan_index_count;
            uint32_t *index_buffer = nullptr;

            MTLTemporaryBuffer allocation =
                context_->get_scratchbuffer_manager().scratch_buffer_allocate_range_aligned(
                    alloc_size, 128);
            index_buffer = (uint32_t *)allocation.data;

            int a = 0;
            for (int i = 0; i < num_triangles; i++) {
              index_buffer[a++] = 0;
              index_buffer[a++] = i + 1;
              index_buffer[a++] = i + 2;
            }

            @autoreleasepool {

              id<MTLBuffer> index_buffer_mtl = nil;
              uint64_t index_buffer_offset = 0;

              /* Region of scratch buffer used for topology emulation element data.
               * NOTE(Metal): We do not need to manually flush as the entire scratch
               * buffer for current command buffer is flushed upon submission. */
              index_buffer_mtl = allocation.metal_buffer;
              index_buffer_offset = allocation.buffer_offset;

              /* Set depth stencil state (requires knowledge of primitive type). */
              context_->ensure_depth_stencil_state(MTLPrimitiveTypeTriangle);

              /* Bind Vertex Buffer. */
              rps.bind_vertex_buffer(
                  current_allocation_.metal_buffer, current_allocation_.buffer_offset, 0);

              /* Draw. */
              [rec drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                              indexCount:fan_index_count
                               indexType:MTLIndexTypeUInt32
                             indexBuffer:index_buffer_mtl
                       indexBufferOffset:index_buffer_offset];
              context_->main_command_buffer.register_draw_counters(fan_index_count);
            }
            rendered = true;
          } break;
          default: {
            BLI_assert_unreachable();
          } break;
        }
      }

      /* If not yet rendered, run through main render path. LineLoop primitive topology emulation
       * will simply amend original data passed into default rendering path. */
      if (!rendered) {
        MTLPrimitiveType primitive_type = metal_primitive_type_;
        int vertex_count = this->vertex_idx;

        /* Bind Vertex Buffer. */
        rps.bind_vertex_buffer(
            current_allocation_.metal_buffer, current_allocation_.buffer_offset, 0);

        /* Set depth stencil state (requires knowledge of primitive type). */
        context_->ensure_depth_stencil_state(primitive_type);

        if (this->shader->is_polyline) {
          this->polyline_draw_workaround(current_allocation_.buffer_offset);
        }
        else {
          /* Regular draw. */
          [rec drawPrimitives:primitive_type vertexStart:0 vertexCount:vertex_count];
          context_->main_command_buffer.register_draw_counters(vertex_count);
        }
      }
    }
    if (G.debug & G_DEBUG_GPU) {
      [rec popDebugGroup];
    }

    if (this->shader->is_polyline) {
      context_->get_scratchbuffer_manager().unbind_as_ssbo();

      context_->pipeline_state.ssbo_bindings[GPU_SSBO_POLYLINE_POS_BUF_SLOT].ssbo = nil;
      context_->pipeline_state.ssbo_bindings[GPU_SSBO_POLYLINE_COL_BUF_SLOT].ssbo = nil;
      context_->pipeline_state.ssbo_bindings[GPU_SSBO_INDEX_BUF_SLOT].ssbo = nil;
      context_->pipeline_state.ssbo_bindings[GPU_SSBO_POLYLINE_POS_BUF_SLOT].bound = false;
      context_->pipeline_state.ssbo_bindings[GPU_SSBO_POLYLINE_COL_BUF_SLOT].bound = false;
      context_->pipeline_state.ssbo_bindings[GPU_SSBO_INDEX_BUF_SLOT].bound = false;
    }
  }

  /* Reset allocation after draw submission. */
  has_begun_ = false;
  if (current_allocation_.metal_buffer) {
    [current_allocation_.metal_buffer release];
    current_allocation_.metal_buffer = nil;
  }
}

}  // namespace blender::gpu
