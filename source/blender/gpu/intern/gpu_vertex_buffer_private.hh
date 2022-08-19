/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2016 by Mike Erwin. All rights reserved. */

/** \file
 * \ingroup gpu
 *
 * GPU vertex buffer
 */

#pragma once

#include "GPU_vertex_buffer.h"

namespace blender::gpu {

/**
 * Implementation of Vertex Buffers.
 * Base class which is then specialized for each implementation (GL, VK, ...).
 */
class VertBuf {
 public:
  static size_t memory_usage;

  GPUVertFormat format = {};
  /** Number of verts we want to draw. */
  uint vertex_len = 0;
  /** Number of verts data. */
  uint vertex_alloc = 0;
  /** Status flag. */
  GPUVertBufStatus flag = GPU_VERTBUF_INVALID;
  /** NULL indicates data in VRAM (unmapped) */
  uchar *data = nullptr;

 protected:
  /** Usage hint for GL optimization. */
  GPUUsageType usage_ = GPU_USAGE_STATIC;

 private:
  /** This counter will only avoid freeing the #GPUVertBuf, not the data. */
  int handle_refcount_ = 1;

 public:
  VertBuf();
  virtual ~VertBuf();

  void init(const GPUVertFormat *format, GPUUsageType usage);
  void clear();

  /* Data management. */
  void allocate(uint vert_len);
  void resize(uint vert_len);
  void upload();
  virtual void bind_as_ssbo(uint binding) = 0;
  virtual void bind_as_texture(uint binding) = 0;

  virtual void wrap_handle(uint64_t handle) = 0;

  VertBuf *duplicate();

  /* Size of the data allocated. */
  size_t size_alloc_get() const
  {
    BLI_assert(format.packed);
    return vertex_alloc * format.stride;
  }
  /* Size of the data uploaded to the GPU. */
  size_t size_used_get() const
  {
    BLI_assert(format.packed);
    return vertex_len * format.stride;
  }

  void reference_add()
  {
    handle_refcount_++;
  }
  void reference_remove()
  {
    BLI_assert(handle_refcount_ > 0);
    handle_refcount_--;
    if (handle_refcount_ == 0) {
      delete this;
    }
  }

  virtual void update_sub(uint start, uint len, const void *data) = 0;
  virtual const void *read() const = 0;
  virtual void *unmap(const void *mapped_data) const = 0;

 protected:
  virtual void acquire_data() = 0;
  virtual void resize_data() = 0;
  virtual void release_data() = 0;
  virtual void upload_data() = 0;
  virtual void duplicate_data(VertBuf *dst) = 0;
};

/* Syntactic sugar. */
static inline GPUVertBuf *wrap(VertBuf *vert)
{
  return reinterpret_cast<GPUVertBuf *>(vert);
}
static inline VertBuf *unwrap(GPUVertBuf *vert)
{
  return reinterpret_cast<VertBuf *>(vert);
}
static inline const VertBuf *unwrap(const GPUVertBuf *vert)
{
  return reinterpret_cast<const VertBuf *>(vert);
}

}  // namespace blender::gpu
