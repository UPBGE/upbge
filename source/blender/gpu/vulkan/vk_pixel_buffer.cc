/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_pixel_buffer.hh"

#include "vk_context.hh"

namespace blender::gpu {

VKPixelBuffer::VKPixelBuffer(size_t size) : PixelBuffer(size)
{
  buffer_.create(size,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 VmaAllocationCreateFlags(0));
  debug::object_label(buffer_.vk_handle(), "PixelBuffer");
}

void *VKPixelBuffer::map()
{
  /* Vulkan buffers are always mapped between allocation and freeing. */
  return buffer_.mapped_memory_get();
}

void VKPixelBuffer::unmap()
{
  /* Vulkan buffers are always mapped between allocation and freeing. */
}

int64_t VKPixelBuffer::get_native_handle()
{
  return int64_t(buffer_.vk_handle());
}

size_t VKPixelBuffer::get_size()
{
  return size_;
}

}  // namespace blender::gpu
