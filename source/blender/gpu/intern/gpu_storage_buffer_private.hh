/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "BLI_sys_types.h"

namespace blender::gpu {

class StorageBuf;
class VertBuf;

#ifndef NDEBUG
#  define DEBUG_NAME_LEN 64
#else
#  define DEBUG_NAME_LEN 8
#endif

/**
 * Implementation of Storage Buffers.
 * Base class which is then specialized for each implementation (GL, VK, ...).
 */
class StorageBuf {
 protected:
  /** Data size in bytes. Doesn't need to match actual allocation size due to alignment rules. */
  size_t size_in_bytes_ = -1;
  size_t usage_size_in_bytes_ = -1;
  /** Continuous memory block to copy to GPU. This data is owned by the StorageBuf. */
  void *data_ = nullptr;
  /** Debugging name */
  char name_[DEBUG_NAME_LEN] = {};

 public:
  StorageBuf(size_t size, const char *name);
  virtual ~StorageBuf();
  void usage_size_set(size_t size);
  size_t usage_size_get() const
  {
    return usage_size_in_bytes_;
  }
  virtual void update(const void *data) = 0;
  virtual void bind(int slot) = 0;
  virtual void unbind() = 0;
  virtual void clear(uint32_t clear_value) = 0;
  virtual void copy_sub(VertBuf *src, uint dst_offset, uint src_offset, uint copy_size) = 0;
  virtual void read(void *data) = 0;
  virtual void async_flush_to_host() = 0;
  virtual void sync_as_indirect_buffer() = 0;

  /**
   * Non-blocking 1-frame-delayed readback for small, frequent reads (e.g. bounding boxes).
   *
   * Each call does two things:
   * 1. If a previous readback request is pending and complete, copies the result into `data`
   *    and returns `true`.
   * 2. Submits a new async copy from the device SSBO to a persistent-mapped readback buffer
   *    for the *next* call to pick up.
   *
   * On the very first call (no previous request), returns `false` and `data` is untouched.
   * This means the data is always 1 frame behind, which is acceptable for bounding boxes.
   *
   * **Key advantage**: avoids the full pipeline flush / GPU stall that `read()` causes.
   * The GPU copy runs asynchronously alongside rendering; by the next frame it is complete.
   *
   * The default implementation falls back to the synchronous `async_flush_to_host()` + `read()`
   * path (returns `true` always, blocking).
   */
  virtual bool read_fast(void *data); //upbge
  /** Return a CPU pointer to the buffer memory when available (Vulkan-only zero-copy). */
  virtual void *mapped_ptr_get() const
  {
    return nullptr;
  }
  /** Opt-in: request the implementation to allocate the buffer as host-visible and persistently
   * mapped. Default no-op for backends that don't support it. Call before allocate(). */
  virtual void enable_host_visible_mapping(){}
};

#undef DEBUG_NAME_LEN

}  // namespace blender::gpu
