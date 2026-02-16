/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "BLI_string.h"

#include "GPU_capabilities.hh"
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
  /* Do not create SSBO GL buffer here to allow allocation from any thread. */
  BLI_assert(size <= GPU_max_storage_buffer_size());
}

void GLStorageBuf::enable_readback_vbo()
{
  use_readback_vbo_ = true;
}

void GLStorageBuf::enable_host_visible_mapping()
{
  /* Opt-in: request the backend to allocate a host-visible persistently
   * mapped readback buffer at allocation time. This mirrors Vulkan's
   * enable_host_visible_mapping semantics as an optional hint for OpenGL. */
  use_host_visible_allocation_ = true;
}

GLStorageBuf::~GLStorageBuf()
{
  if (read_fence_) {
    glDeleteSync(read_fence_);
  }

  if (persistent_ptr_) {
    if (GLContext::direct_state_access_support) {
      if (use_readback_vbo_)
        glUnmapNamedBuffer(read_vbo_id_);
      else
        glUnmapNamedBuffer(read_ssbo_id_);
    }
    else {
      if (use_readback_vbo_) {
        glBindBuffer(GL_ARRAY_BUFFER, read_vbo_id_);
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
      }
      else {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, read_ssbo_id_);
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
      }
    }
  }

  if (read_ssbo_id_) {
    GLContext::buffer_free(read_ssbo_id_);
  }
  if (read_vbo_id_) {
    GLContext::buffer_free(read_vbo_id_);
  }

  GLContext::buffer_free(ssbo_id_);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Data upload / update
 * \{ */

void GLStorageBuf::init()
{
  BLI_assert(GLContext::get());

  alloc_size_in_bytes_ = ceil_to_multiple_ul(size_in_bytes_, 16);
  if (GLContext::direct_state_access_support) {
    glCreateBuffers(1, &ssbo_id_);
    glNamedBufferData(ssbo_id_, alloc_size_in_bytes_, nullptr, to_gl(this->usage_));
  }
  else {
    glGenBuffers(1, &ssbo_id_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_id_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, alloc_size_in_bytes_, nullptr, to_gl(this->usage_));
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  }

  debug::object_label(GL_SHADER_STORAGE_BUFFER, ssbo_id_, name_);

  /* If requested, create and persistently map the readback buffer now to avoid
   * incurring that cost at first read. Use the actual useful size (size_in_bytes_). */
  if (use_host_visible_allocation_) {
    if (read_ssbo_id_ == 0) {
      if (GLContext::direct_state_access_support) {
        glCreateBuffers(1, &read_ssbo_id_);
        glNamedBufferStorage(read_ssbo_id_,
                             size_in_bytes_,
                             nullptr,
                             GL_MAP_PERSISTENT_BIT | GL_MAP_READ_BIT |
                                 GL_MAP_COHERENT_BIT);
        persistent_ptr_ = glMapNamedBufferRange(read_ssbo_id_,
                                                0,
                                                size_in_bytes_,
                                                GL_MAP_PERSISTENT_BIT | GL_MAP_READ_BIT | GL_MAP_COHERENT_BIT);
        BLI_assert(persistent_ptr_);
        debug::object_label(GL_SHADER_STORAGE_BUFFER, read_ssbo_id_, name_);
      }
      else {
        glGenBuffers(1, &read_ssbo_id_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, read_ssbo_id_);
        glBufferStorage(GL_SHADER_STORAGE_BUFFER,
                        size_in_bytes_,
                        nullptr,
                        GL_MAP_PERSISTENT_BIT | GL_MAP_READ_BIT | GL_MAP_COHERENT_BIT);
        persistent_ptr_ = glMapBufferRange(GL_SHADER_STORAGE_BUFFER,
                                           0,
                                           size_in_bytes_,
                                           GL_MAP_PERSISTENT_BIT | GL_MAP_READ_BIT | GL_MAP_COHERENT_BIT);
        BLI_assert(persistent_ptr_);
        debug::object_label(GL_SHADER_STORAGE_BUFFER, read_ssbo_id_, name_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
      }
    }
  }
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
    MEM_SAFE_DELETE_VOID(data_);
  }

  slot_ = slot;
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot_, ssbo_id_);

#ifndef NDEBUG
  BLI_assert(slot < 16);
  GLContext::get()->bound_ssbo_slots |= 1 << slot;
#endif
}

void GLStorageBuf::bind_as(GLenum target)
{
  BLI_assert_msg(ssbo_id_ != 0,
                 "Trying to use storage buffer as indirect buffer but buffer was never filled.");
  glBindBuffer(target, ssbo_id_);
}

void GLStorageBuf::unbind()
{
#ifndef NDEBUG
  /* NOTE: This only unbinds the last bound slot. */
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot_, 0);
  /* Hope that the context did not change. */
  GLContext::get()->bound_ssbo_slots &= ~(1 << slot_);
#endif
  slot_ = 0;
}

void GLStorageBuf::clear(uint32_t clear_value)
{
  if (ssbo_id_ == 0) {
    this->init();
  }

  if (GLContext::direct_state_access_support) {
    glClearNamedBufferData(ssbo_id_, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &clear_value);
  }
  else {
    /* WATCH(@fclem): This should be ok since we only use clear outside of drawing functions. */
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_id_);
    glClearBufferData(
        GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &clear_value);
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

void GLStorageBuf::async_flush_to_host()
{
  if (ssbo_id_ == 0) {
    this->init();
  }

  if (read_ssbo_id_ == 0) {
    glGenBuffers(1, &read_ssbo_id_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, read_ssbo_id_);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER,
                    alloc_size_in_bytes_,
                    nullptr,
                    GL_MAP_PERSISTENT_BIT | GL_MAP_READ_BIT | GL_MAP_COHERENT_BIT);
    persistent_ptr_ = glMapBufferRange(GL_SHADER_STORAGE_BUFFER,
                                       0,
                                       alloc_size_in_bytes_,
                                       GL_MAP_PERSISTENT_BIT | GL_MAP_READ_BIT | GL_MAP_COHERENT_BIT);
    BLI_assert(persistent_ptr_);
    debug::object_label(GL_SHADER_STORAGE_BUFFER, read_ssbo_id_, name_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  }

  if (use_readback_vbo_) {
    if (GLContext::direct_state_access_support) {
      glCopyNamedBufferSubData(ssbo_id_, read_vbo_id_, 0, 0, size_in_bytes_);
    }
    else {
      glBindBuffer(GL_COPY_READ_BUFFER, ssbo_id_);
      glBindBuffer(GL_COPY_WRITE_BUFFER, read_vbo_id_);
      glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, size_in_bytes_);
      glBindBuffer(GL_COPY_READ_BUFFER, 0);
      glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    }
  }
  else {
    if (GLContext::direct_state_access_support) {
      glCopyNamedBufferSubData(ssbo_id_, read_ssbo_id_, 0, 0, size_in_bytes_);
    }
    else {
      glBindBuffer(GL_COPY_READ_BUFFER, ssbo_id_);
      glBindBuffer(GL_COPY_WRITE_BUFFER, read_ssbo_id_);
      glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, size_in_bytes_);
      glBindBuffer(GL_COPY_READ_BUFFER, 0);
      glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    }
  }

  glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);

  if (read_fence_) {
    glDeleteSync(read_fence_);
  }
  read_fence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

void GLStorageBuf::read(void *data)
{
  if (data == nullptr) {
    return;
  }

  if (!read_fence_) {
    /* Synchronous path. */
    if (GLContext::direct_state_access_support) {
      glGetNamedBufferSubData(ssbo_id_, 0, size_in_bytes_, data);
    }
    else {
      glBindBuffer(GL_COPY_READ_BUFFER, ssbo_id_);
      glGetBufferSubData(GL_COPY_READ_BUFFER, 0, size_in_bytes_, data);
      glBindBuffer(GL_COPY_READ_BUFFER, 0);
    }
    return;
  }

  while (glClientWaitSync(read_fence_, GL_SYNC_FLUSH_COMMANDS_BIT, 1000) == GL_TIMEOUT_EXPIRED) {
    /* Repeat until the data is ready. */
  }
  glDeleteSync(read_fence_);
  read_fence_ = nullptr;

  BLI_assert(persistent_ptr_);
  memcpy(data, persistent_ptr_, size_in_bytes_);
}

// upbge
bool GLStorageBuf::read_fast(void *data)
{
  if (data == nullptr) {
    return false;
  }
  if (ssbo_id_ == 0) {
    this->init();
  }

  /* Ensure the persistent-mapped readback buffer exists (coherent = GPU writes auto-visible). */
  if (read_ssbo_id_ == 0) {
    glGenBuffers(1, &read_ssbo_id_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, read_ssbo_id_);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER,
                    size_in_bytes_,
                    nullptr,
                    GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_MAP_READ_BIT);
    persistent_ptr_ = glMapBufferRange(GL_SHADER_STORAGE_BUFFER,
                                       0,
                                       size_in_bytes_,
                                       GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT |
                                           GL_MAP_READ_BIT);
    BLI_assert(persistent_ptr_);
    debug::object_label(GL_SHADER_STORAGE_BUFFER, read_ssbo_id_, name_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  }

  bool has_result = false;

  /* Step 1: If a fence from a PREVIOUS call is pending, check if it's done.
   * By now (next frame) it should be signaled — zero or near-zero wait. */
  if (read_fence_) {
    GLenum status = glClientWaitSync(read_fence_, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
    if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
      /* Previous copy is complete — read from the persistent pointer or read_vbo_id_. */
      if (read_vbo_id_) {
        memcpy(data, persistent_ptr_, size_in_bytes_);
      } else {
        glBindBuffer(GL_COPY_READ_BUFFER, read_vbo_id_);
        glGetBufferSubData(GL_COPY_READ_BUFFER, 0, size_in_bytes_, data);
        glBindBuffer(GL_COPY_READ_BUFFER, 0);
      }
      has_result = true;
      glDeleteSync(read_fence_);
      read_fence_ = nullptr;
    }
    else {
      /* Still not ready (unlikely after a full frame). Don't wait — skip this readback.
       * The fence stays alive; we'll check again next frame. */
      return false;
    }
  }

  /* Step 2: Submit a NEW async copy for the next call to pick up. */
  if (GLContext::direct_state_access_support) {
    glCopyNamedBufferSubData(ssbo_id_, read_ssbo_id_, 0, 0, size_in_bytes_);
  }
  else {
    glBindBuffer(GL_COPY_READ_BUFFER, ssbo_id_);
    glBindBuffer(GL_COPY_WRITE_BUFFER, read_ssbo_id_);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, size_in_bytes_);
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
  }

  read_fence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

  return has_result;
}

void GLStorageBuf::sync_as_indirect_buffer()
{
  bind_as(GL_DRAW_INDIRECT_BUFFER);
  glMemoryBarrier(GL_COMMAND_BARRIER_BIT);
  glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
}

/** \} */

}  // namespace blender::gpu
