/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2020 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup gpu
 */

#include "gl_context.hh"

#include "gl_index_buffer.hh"

namespace blender::gpu {

GLIndexBuf::~GLIndexBuf()
{
  GLContext::buf_free(ibo_id_);
}

void GLIndexBuf::bind()
{
  if (is_subrange_) {
    static_cast<GLIndexBuf *>(src_)->bind();
    return;
  }

  const bool allocate_on_device = ibo_id_ == 0;
  if (allocate_on_device) {
    glGenBuffers(1, &ibo_id_);
  }

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_id_);

  if (data_ != nullptr || allocate_on_device) {
    size_t size = this->size_get();
    /* Sends data to GPU. */
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, size, data_, GL_STATIC_DRAW);
    /* No need to keep copy of data in system memory. */
    MEM_SAFE_FREE(data_);
  }
}

void GLIndexBuf::bind_as_ssbo(uint binding)
{
  bind();
  BLI_assert(ibo_id_ != 0);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, ibo_id_);
}

const uint32_t *GLIndexBuf::read() const
{
  BLI_assert(is_active());
  void *data = glMapBuffer(GL_ELEMENT_ARRAY_BUFFER, GL_READ_ONLY);
  uint32_t *result = static_cast<uint32_t *>(data);
  return result;
}

bool GLIndexBuf::is_active() const
{
  if (!ibo_id_) {
    return false;
  }
  int active_ibo_id = 0;
  glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &active_ibo_id);
  return ibo_id_ == active_ibo_id;
}

void GLIndexBuf::upload_data()
{
  bind();
}

void GLIndexBuf::update_sub(uint start, uint len, const void *data)
{
  glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, start, len, data);
}

}  // namespace blender::gpu
