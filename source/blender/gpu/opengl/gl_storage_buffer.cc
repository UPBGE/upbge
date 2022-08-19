/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2022 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup gpu
 */

#include "BLI_string.h"

#include "gpu_backend.hh"
#include "gpu_context_private.hh"

#include "gl_backend.hh"
#include "gl_debug.hh"
#include "gl_storage_buffer.hh"
#include "gl_vertex_buffer.hh"

namespace blender::gpu {

/* -------------------------------------------------------------------- */
/** \name Creation & Deletion
 * \{ */

GLStorageBuf::GLStorageBuf(size_t size, GPUUsageType usage, const char *name)
    : StorageBuf(size, name)
{
  usage_ = usage;
  /* Do not create UBO GL buffer here to allow allocation from any thread. */
  BLI_assert(size <= GLContext::max_ssbo_size);
}

GLStorageBuf::~GLStorageBuf()
{
  GLContext::buf_free(ssbo_id_);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Data upload / update
 * \{ */

void GLStorageBuf::init()
{
  BLI_assert(GLContext::get());

  glGenBuffers(1, &ssbo_id_);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_id_);
  glBufferData(GL_SHADER_STORAGE_BUFFER, size_in_bytes_, nullptr, to_gl(this->usage_));

  debug::object_label(GL_SHADER_STORAGE_BUFFER, ssbo_id_, name_);
}

void GLStorageBuf::update(const void *data)
{
  if (ssbo_id_ == 0) {
    this->init();
  }
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_id_);
  glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, size_in_bytes_, data);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Usage
 * \{ */

void GLStorageBuf::bind(int slot)
{
  if (slot >= GLContext::max_ssbo_binds) {
    fprintf(
        stderr,
        "Error: Trying to bind \"%s\" ssbo to slot %d which is above the reported limit of %d.\n",
        name_,
        slot,
        GLContext::max_ssbo_binds);
    return;
  }

  if (ssbo_id_ == 0) {
    this->init();
  }

  if (data_ != nullptr) {
    this->update(data_);
    MEM_SAFE_FREE(data_);
  }

  slot_ = slot;
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot_, ssbo_id_);

#ifdef DEBUG
  BLI_assert(slot < 16);
  /* TODO */
  // GLContext::get()->bound_ssbo_slots |= 1 << slot;
#endif
}

void GLStorageBuf::bind_as(GLenum target)
{
  BLI_assert_msg(ssbo_id_ != 0,
                 "Trying to use storage buf as indirect buffer but buffer was never filled.");
  glBindBuffer(target, ssbo_id_);
}

void GLStorageBuf::unbind()
{
#ifdef DEBUG
  /* NOTE: This only unbinds the last bound slot. */
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot_, 0);
  /* Hope that the context did not change. */
  /* TODO */
  // GLContext::get()->bound_ssbo_slots &= ~(1 << slot_);
#endif
  slot_ = 0;
}

void GLStorageBuf::clear(eGPUTextureFormat internal_format, eGPUDataFormat data_format, void *data)
{
  if (ssbo_id_ == 0) {
    this->init();
  }

  if (GLContext::direct_state_access_support) {
    glClearNamedBufferData(ssbo_id_,
                           to_gl_internal_format(internal_format),
                           to_gl_data_format(internal_format),
                           to_gl(data_format),
                           data);
  }
  else {
    /* WATCH(@fclem): This should be ok since we only use clear outside of drawing functions. */
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_id_);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER,
                      to_gl_internal_format(internal_format),
                      to_gl_data_format(internal_format),
                      to_gl(data_format),
                      data);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  }
}

void GLStorageBuf::copy_sub(VertBuf *src_, uint dst_offset, uint src_offset, uint copy_size)
{
  GLVertBuf *src = static_cast<GLVertBuf *>(src_);
  GLStorageBuf *dst = this;

  if (dst->ssbo_id_ == 0) {
    dst->init();
  }
  if (src->vbo_id_ == 0) {
    src->bind();
  }

  if (GLContext::direct_state_access_support) {
    glCopyNamedBufferSubData(src->vbo_id_, dst->ssbo_id_, src_offset, dst_offset, copy_size);
  }
  else {
    /* This binds the buffer to GL_ARRAY_BUFFER and upload the data if any. */
    src->bind();
    glBindBuffer(GL_COPY_WRITE_BUFFER, dst->ssbo_id_);
    glCopyBufferSubData(GL_ARRAY_BUFFER, GL_COPY_WRITE_BUFFER, src_offset, dst_offset, copy_size);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
  }
}

/** \} */

}  // namespace blender::gpu
