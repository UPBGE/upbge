/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */
#include "vk_shader.hh"
#include "vk_shader_interface.hh"
#include "vk_staging_buffer.hh"
#include "vk_state_manager.hh"
#include "vk_vertex_buffer.hh"

#include "vk_storage_buffer.hh"

#include "CLG_log.h"

namespace blender {

static CLG_LogRef LOG = {"gpu.vulkan"};

namespace gpu {

VKStorageBuffer::VKStorageBuffer(size_t size, GPUUsageType usage, const char *name)
    : StorageBuf(size, name), usage_(usage)
{
  UNUSED_VARS(usage_);
}
VKStorageBuffer::~VKStorageBuffer()
{
  if (async_read_buffer_) {
    MEM_delete(async_read_buffer_);
    async_read_buffer_ = nullptr;
  }
  if (fast_read_buffer_) {
    MEM_delete(fast_read_buffer_);
    fast_read_buffer_ = nullptr;
  }
}

void *VKStorageBuffer::mapped_ptr_get() const
{
  /* By default we don't allocate host-visible storage buffers. Return nullptr. */
  return buffer_.is_mapped() ? buffer_.mapped_memory_get() : nullptr;
}

void VKStorageBuffer::update(const void *data)
{
  VKContext &context = *VKContext::get();
  ensure_allocated();
  if (!buffer_.is_allocated()) {
    CLOG_WARN(&LOG,
              "Unable to upload data to storage buffer as the storage buffer could not be "
              "allocated on GPU.");
    return;
  }

  if (usage_ == GPU_USAGE_STREAM) {
    const VKDevice &device = VKBackend::get().device;
    VKStreamingBuffer &streaming_buffer = *context.get_or_create_streaming_buffer(
        buffer_, device.physical_device_properties_get().limits.minStorageBufferOffsetAlignment);
    offset_ = streaming_buffer.update(context, data, usage_size_in_bytes_);
    return;
  }

  VKStagingBuffer staging_buffer(
      buffer_, VKStagingBuffer::Direction::HostToDevice, 0, usage_size_in_bytes_);
  VKBuffer &buffer = staging_buffer.host_buffer_get();
  if (buffer.is_allocated()) {
    buffer.update_immediately(data);
    staging_buffer.copy_to_device(context);
  }
  else {
    CLOG_ERROR(
        &LOG,
        "Unable to upload data to storage buffer via a staging buffer as the staging buffer "
        "could not be allocated. Storage buffer will be filled with on zeros to reduce "
        "drawing artifacts due to read from uninitialized memory.");
    buffer_.clear(context, 0u);
  }
}

void VKStorageBuffer::ensure_allocated()
{
  if (!buffer_.is_allocated()) {
    allocate();
  }
}

void VKStorageBuffer::allocate()
{
  const VkBufferUsageFlags buffer_usage_flags = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                VK_BUFFER_USAGE_TRANSFER_DST_BIT;

  VmaMemoryUsage vma_usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  VmaAllocationCreateFlags alloc_flags = VmaAllocationCreateFlags(0);
  if (use_host_visible_allocation_) {
    /* Prefer host visible memory and request mapping flags for VMA. */
    vma_usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    alloc_flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                   VMA_ALLOCATION_CREATE_MAPPED_BIT;
  }

  buffer_.create(size_in_bytes_, buffer_usage_flags, vma_usage, alloc_flags, 0.8f);
  if (buffer_.is_allocated()) {
    debug::object_label(buffer_.vk_handle(), name_);
  }
}

void VKStorageBuffer::bind(int slot)
{
  VKContext &context = *VKContext::get();
  context.state_manager_get().storage_buffer_bind(
      BindSpaceStorageBuffers::Type::StorageBuffer, this, slot, offset_);
}

void VKStorageBuffer::unbind()
{
  VKContext *context = VKContext::get();
  if (context) {
    context->state_manager_get().storage_buffer_unbind(this);
  }
}

void VKStorageBuffer::clear(uint32_t clear_value)
{
  ensure_allocated();
  VKContext &context = *VKContext::get();
  buffer_.clear(context, clear_value);
}

void VKStorageBuffer::copy_sub(VertBuf *src, uint dst_offset, uint src_offset, uint copy_size)
{
  ensure_allocated();

  VKVertexBuffer &src_vertex_buffer = *unwrap(src);
  src_vertex_buffer.upload();

  render_graph::VKCopyBufferNode::CreateInfo copy_buffer = {};
  copy_buffer.src_buffer = src_vertex_buffer.vk_handle();
  copy_buffer.dst_buffer = vk_handle();
  copy_buffer.region.srcOffset = src_offset;
  copy_buffer.region.dstOffset = dst_offset;
  copy_buffer.region.size = copy_size;

  VKContext &context = *VKContext::get();
  context.render_graph().add_node(copy_buffer);
}

void VKStorageBuffer::async_flush_to_host()
{
  if (async_read_buffer_ != nullptr) {
    return;
  }
  ensure_allocated();
  VKContext &context = *VKContext::get();

  async_read_buffer_ = MEM_new<VKStagingBuffer>(
      __func__, buffer_, VKStagingBuffer::Direction::DeviceToHost);
  async_read_buffer_->copy_from_device(context);
  async_read_buffer_->host_buffer_get().async_flush_to_host(context);
}

void VKStorageBuffer::read(void *data)
{
  if (async_read_buffer_ == nullptr) {
    async_flush_to_host();
  }

  VKContext &context = *VKContext::get();
  async_read_buffer_->host_buffer_get().read_async(context, data);
  MEM_delete(async_read_buffer_);
  async_read_buffer_ = nullptr;
}

// upbge
bool VKStorageBuffer::read_fast(void *data)
{
  ensure_allocated();
  VKContext &context = *VKContext::get();

  /* If the backing buffer is host-visible and persistently mapped we can avoid the
   * staging buffer entirely: wait for the previous timeline, invalidate the mapped range
   * and memcpy from the mapped memory. This is Vulkan-only zero-copy path (no staging). */
  void *mapped = mapped_ptr_get();
  if (mapped != nullptr) {
    bool has_result = false;
    if (fast_read_timeline_ != 0) {
      VKDevice &device = VKBackend::get().device;
      device.wait_for_timeline(fast_read_timeline_);
      fast_read_timeline_ = 0;

      /* Make device writes visible to the host. */
      buffer_.invalidate_mapped_range(0, size_in_bytes_);
      memcpy(data, mapped, size_in_bytes_);
      has_result = true;
    }

    /* Ensure rendering is flushed so the next frame's data will be available. */
    context.rendering_end();
    fast_read_timeline_ = context.flush_render_graph(RenderGraphFlushFlags::SUBMIT |
                                                     RenderGraphFlushFlags::RENEW_RENDER_GRAPH);

    return has_result;
  }

  /* Fallback to existing staging-based implementation. */
  /* Create a dedicated host-visible staging buffer that persists across frames. */
  if (fast_read_buffer_ == nullptr) {
    fast_read_buffer_ = MEM_new<VKStagingBuffer>(
        __func__, buffer_, VKStagingBuffer::Direction::DeviceToHost);
  }

  bool has_result = false;

  /* Step 1: If a previous submission is pending, check if it has completed.
   * After a full frame of rendering, the timeline should have advanced past our value. */
  if (fast_read_timeline_ != 0) {
    VKDevice &device = VKBackend::get().device;
    /* Non-blocking check: timeline should already be past this value after a full frame. */
    device.wait_for_timeline(fast_read_timeline_);
    fast_read_timeline_ = 0;

    /* The staging buffer is mapped — just memcpy. */
    BLI_assert(fast_read_buffer_->host_buffer_get().is_mapped());
    memcpy(data, fast_read_buffer_->host_buffer_get().mapped_memory_get(), size_in_bytes_);
    has_result = true;
  }

  /* Step 2: Submit a new async copy for the next call to pick up. */
  fast_read_buffer_->copy_from_device(context);

  /* Flush the render graph to actually submit the copy command, but do NOT wait. */
  context.rendering_end();
  fast_read_timeline_ = context.flush_render_graph(RenderGraphFlushFlags::SUBMIT |
                                                   RenderGraphFlushFlags::RENEW_RENDER_GRAPH);

  return has_result;
}

}  // namespace gpu
}  // namespace blender
