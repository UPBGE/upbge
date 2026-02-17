/* SPDX-FileCopyrightText: 2016 by Mike Erwin. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "GPU_texture.hh"

#include "gl_context.hh"

#include "gl_vertex_buffer.hh"
#include "gl_debug.hh"

namespace blender::gpu {

void GLVertBuf::acquire_data()
{
  if (usage_ == GPU_USAGE_DEVICE_ONLY) {
    return;
  }

  /* Discard previous data if any. */
  MEM_SAFE_DELETE(data_);
  data_ = MEM_new_array_uninitialized<uchar>(this->size_alloc_get(), __func__);
}

void GLVertBuf::resize_data()
{
  if (usage_ == GPU_USAGE_DEVICE_ONLY) {
    return;
  }

  data_ = static_cast<uchar *>(
      MEM_realloc_uninitialized(data_, sizeof(uchar) * this->size_alloc_get()));
}

void GLVertBuf::release_data()
{
  if (is_wrapper_) {
    return;
  }

  if (read_fence_) {
    GLContext::get()->fence_free(read_fence_);
    read_fence_ = 0;
  }
  if (read_vbo_id_ != 0) {
    GLContext::buffer_free(read_vbo_id_);
    read_vbo_id_ = 0;
  }

  if (vbo_id_ != 0) {
    GPU_TEXTURE_FREE_SAFE(buffer_texture_);
    GLContext::buffer_free(vbo_id_);
    vbo_id_ = 0;
    memory_usage -= vbo_size_;
  }

  MEM_SAFE_DELETE(data_);
}

void GLVertBuf::upload_data()
{
  this->bind();
}

void GLVertBuf::bind()
{
  BLI_assert(GLContext::get() != nullptr);

  if (vbo_id_ == 0) {
    glGenBuffers(1, &vbo_id_);
  }

  glBindBuffer(GL_ARRAY_BUFFER, vbo_id_);

  if (flag & GPU_VERTBUF_DATA_DIRTY) {
    vbo_size_ = this->size_used_get();

    /* This is fine on some systems but will crash on others. */
    BLI_assert(vbo_size_ != 0);
    /* Orphan the vbo to avoid sync then upload data. */
    glBufferData(GL_ARRAY_BUFFER, ceil_to_multiple_ul(vbo_size_, 16), nullptr, to_gl(usage_));
    /* Do not transfer data from host to device when buffer is device only. */
    if (usage_ != GPU_USAGE_DEVICE_ONLY) {
      glBufferSubData(GL_ARRAY_BUFFER, 0, vbo_size_, data_);
    }
    memory_usage += vbo_size_;

    if (usage_ == GPU_USAGE_STATIC) {
      MEM_SAFE_DELETE(data_);
    }
    flag &= ~GPU_VERTBUF_DATA_DIRTY;
    flag |= GPU_VERTBUF_DATA_UPLOADED;
  }
}

// upbge
bool GLVertBuf::read_fast(void *data)
{
  if (data == nullptr) {
    return false;
  }

  /* Ensure the persistent-mapped readback buffer exists (coherent = GPU writes auto-visible). */
  if (read_vbo_id_ == 0) {
    glGenBuffers(1, &read_vbo_id_);
    const size_t alloc_size = ceil_to_multiple_ul((size_t)vbo_size_, 16);
    glBindBuffer(GL_ARRAY_BUFFER, read_vbo_id_);
    glBufferStorage(GL_ARRAY_BUFFER,
                    alloc_size,
                    nullptr,
                    GL_MAP_PERSISTENT_BIT | GL_MAP_READ_BIT | GL_MAP_COHERENT_BIT);
    persistent_ptr_ = glMapBufferRange(GL_ARRAY_BUFFER,
                                       0,
                                       alloc_size,
                                       GL_MAP_PERSISTENT_BIT |
                                           GL_MAP_READ_BIT |
                                           GL_MAP_COHERENT_BIT);
    BLI_assert(persistent_ptr_);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
  }

  bool has_result = false;

  /* Step 1: If a fence from a PREVIOUS call is pending, check if it's done.
   * By now (next frame) it should be signaled — zero or near-zero wait. */
  if (read_fence_) {
    GLenum status = glClientWaitSync(read_fence_, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
    if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
      /* Previous copy is complete — read from the persistent pointer. */
      memcpy(data, persistent_ptr_, vbo_size_);
      has_result = true;
      GLContext::get()->fence_free(read_fence_);
      read_fence_ = 0;
    }
    else {
      /* Still not ready (unlikely after a full frame). Don't wait — skip this readback.
       * The fence stays alive; we'll check again next frame. */
      return false;
    }
  }

  /* Step 2: Submit a NEW async copy for the next call to pick up. */
  if (GLContext::direct_state_access_support) {
    glCopyNamedBufferSubData(vbo_id_, read_vbo_id_, 0, 0, vbo_size_);
  }
  else {
    glBindBuffer(GL_COPY_READ_BUFFER, vbo_id_);
    glBindBuffer(GL_COPY_WRITE_BUFFER, read_vbo_id_);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, vbo_size_);
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
  }

  read_fence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

  return has_result;
}

void GLVertBuf::bind_as_ssbo(uint binding)
{
  bind();
  BLI_assert(vbo_id_ != 0);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, vbo_id_);

#ifndef NDEBUG
  BLI_assert(binding < 16);
  GLContext::get()->bound_ssbo_slots |= 1 << binding;
#endif
}

void GLVertBuf::bind_as_texture(uint binding)
{
  bind();
  BLI_assert(vbo_id_ != 0);
  if (buffer_texture_ == nullptr) {
    buffer_texture_ = GPU_texture_create_from_vertbuf("vertbuf_as_texture", this);
  }
  GPU_texture_bind(buffer_texture_, binding);
}

void GLVertBuf::read(void *data) const
{
  if (data == nullptr) {
    return;
  }

  if (vbo_id_ == 0) {
    return;
  }

  /* Prefer DSA path when available to avoid changing binding state. */
  if (GLContext::direct_state_access_support) {
    GLsizeiptr sz = (GLsizeiptr)size_used_get();
    glGetNamedBufferSubData(vbo_id_, 0, sz, data);
    return;
  }

  /* Fallback: bind the buffer, read, and restore previous binding. */
  GLint prev = 0;
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev);
  glBindBuffer(GL_ARRAY_BUFFER, vbo_id_);
  glGetBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)size_used_get(), data);
  glBindBuffer(GL_ARRAY_BUFFER, prev);
}

void GLVertBuf::wrap_handle(uint64_t handle)
{
  BLI_assert(vbo_id_ == 0);
  BLI_assert(glIsBuffer(uint(handle)));
  is_wrapper_ = true;
  vbo_id_ = uint(handle);
  /* We assume the data is already on the device, so no need to allocate or send it. */
  flag = GPU_VERTBUF_DATA_UPLOADED;
}

bool GLVertBuf::is_active() const
{
  if (!vbo_id_) {
    return false;
  }
  int active_vbo_id = 0;
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &active_vbo_id);
  return vbo_id_ == active_vbo_id;
}

void GLVertBuf::update_sub(uint start, uint len, const void *data)
{
  glBufferSubData(GL_ARRAY_BUFFER, start, len, data);
}

}  // namespace blender::gpu
