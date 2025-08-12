/* SPDX-FileCopyrightText: 2016 by Mike Erwin. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "GPU_vertex_buffer.hh"
#include "gpu_shader_interface.hh"

#include "gl_batch.hh"
#include "gl_context.hh"
#include "gl_index_buffer.hh"
#include "gl_storage_buffer.hh"
#include "gl_vertex_buffer.hh"

#include "gl_vertex_array.hh"

namespace blender::gpu {

/* -------------------------------------------------------------------- */
/** \name Vertex Array Bindings
 * \{ */

/** Returns enabled vertex pointers as a bit-flag (one bit per attribute). */
static uint16_t vbo_bind(const ShaderInterface *interface,
                         const GPUVertFormat *format,
                         uint v_first,
                         uint v_len,
                         const bool use_instancing)
{
  uint16_t enabled_attrib = 0;
  const uint attr_len = format->attr_len;
  uint stride = format->stride;
  uint offset = 0;
  GLuint divisor = (use_instancing) ? 1 : 0;

  for (uint a_idx = 0; a_idx < attr_len; a_idx++) {
    const GPUVertAttr *a = &format->attrs[a_idx];

    if (format->deinterleaved) {
      offset += ((a_idx == 0) ? 0 : format->attrs[a_idx - 1].type.size()) * v_len;
      stride = a->type.size();
    }
    else {
      offset = a->offset;
    }

    /* This is in fact an offset in memory. */
    const GLvoid *pointer = (const GLubyte *)intptr_t(offset + v_first * stride);
    const GLenum type = to_gl(a->type.comp_type());

    for (uint n_idx = 0; n_idx < a->name_len; n_idx++) {
      const char *name = GPU_vertformat_attr_name_get(format, a, n_idx);
      const ShaderInput *input = interface->attr_get(name);

      if (input == nullptr || input->location == -1) {
        continue;
      }

      enabled_attrib |= (1 << input->location);

      glEnableVertexAttribArray(input->location);
      glVertexAttribDivisor(input->location, divisor);

      switch (a->type.fetch_mode()) {
        case GPU_FETCH_FLOAT:
        case GPU_FETCH_INT_TO_FLOAT_UNIT:
          glVertexAttribPointer(
              input->location, a->type.comp_len(), type, GL_TRUE, stride, pointer);
          break;
        case GPU_FETCH_INT:
          glVertexAttribIPointer(input->location, a->type.comp_len(), type, stride, pointer);
          break;
      }
    }
  }
  return enabled_attrib;
}

void GLVertArray::update_bindings(const GLuint vao,
                                  const Batch *batch_, /* Should be GLBatch. */
                                  const ShaderInterface *interface,
                                  const int base_instance)
{
  const GLBatch *batch = static_cast<const GLBatch *>(batch_);
  uint16_t attr_mask = interface->enabled_attr_mask_;

  glBindVertexArray(vao);

  /* Reverse order so first VBO'S have more prevalence (in term of attribute override). */
  for (int v = GPU_BATCH_VBO_MAX_LEN - 1; v > -1; v--) {
    GLVertBuf *vbo = batch->verts_(v);
    if (vbo) {
      vbo->bind();
      attr_mask &= ~vbo_bind(interface, &vbo->format, 0, vbo->vertex_len, false);
    }
  }

  for (int v = GPU_BATCH_INST_VBO_MAX_LEN - 1; v > -1; v--) {
    GLVertBuf *vbo = batch->inst_(v);
    if (vbo) {
      vbo->bind();
      attr_mask &= ~vbo_bind(interface, &vbo->format, base_instance, vbo->vertex_len, true);
    }
  }

  if (attr_mask != 0) {
    for (uint16_t mask = 1, a = 0; a < 16; a++, mask <<= 1) {
      if (attr_mask & mask) {
        GLContext *ctx = GLContext::get();
        /* This replaces glVertexAttrib4f(a, 0.0f, 0.0f, 0.0f, 1.0f); with a more modern style.
         * Fix issues for some drivers (see #75069). */
        glBindVertexBuffer(a, ctx->default_attr_vbo_, intptr_t(0), intptr_t(0));
        glEnableVertexAttribArray(a);
        glVertexAttribFormat(a, 4, GL_FLOAT, GL_FALSE, 0);
        glVertexAttribBinding(a, a);
      }
    }
  }

  if (batch->elem) {
    /* Binds the index buffer. This state is also saved in the VAO. */
    static_cast<GLIndexBuf *>(batch->elem)->bind();
  }
}

void GLVertArray::update_bindings(const GLuint vao,
                                  const uint v_first,
                                  const GPUVertFormat *format,
                                  const ShaderInterface *interface)
{
  glBindVertexArray(vao);

  vbo_bind(interface, format, v_first, 0, false);
}

/** \} */

}  // namespace blender::gpu
