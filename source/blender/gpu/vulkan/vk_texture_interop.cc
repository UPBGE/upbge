/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Implementation of the Vulkan interoperability functions declared in GPU_texture.hh.
 * Lives inside source/blender/gpu/vulkan/ so it can freely include internal headers.
 *
 * This is the ONLY translation unit in the codebase that casts gpu::Texture* to
 * VKTexture* for the purpose of exposing Vulkan handles to external callers.
 */

#include "GPU_context.hh"   /* GPU_backend_get_type() */
#include "GPU_texture.hh"   /* GPUTextureVKHandles, GPUVKDeviceHandles */

#include "vk_backend.hh"
#include "vk_common.hh"     /* VkFormat, VkImage, etc. (via <vulkan/vulkan.h>) */
#include "vk_device.hh"
#include "vk_image_view.hh"
#include "vk_texture.hh"

namespace blender {

/* ------------------------------------------------------------------ */

GPUTextureVKHandles GPU_texture_vk_handles_get(gpu::Texture *texture)
{
  GPUTextureVKHandles result = {};

  if (GPU_backend_get_type() != GPU_BACKEND_VULKAN || texture == nullptr) {
    return result;
  }

  gpu::VKTexture &vk_tex = *gpu::unwrap(texture);

  /* vk_image_handle() asserts internally if the image is VK_NULL_HANDLE,
   * which catches unallocated textures in debug builds. */
  const VkImage vk_image = vk_tex.vk_image_handle();
  if (vk_image == VK_NULL_HANDLE) {
    return result;
  }

  /* Retrieve the default shader-binding image view:
   *   - DONT_CARE: let the backend decide arrayed vs non-arrayed
   *   - DEFAULT:   full mip range, full layer range, standard swizzle
   * This is the view FSR3 needs for its compute passes. */
  const gpu::VKImageView &img_view = vk_tex.image_view_get(
      gpu::VKImageViewArrayed::DONT_CARE,
      gpu::VKImageViewFlags::DEFAULT);
  const VkImageView vk_view = img_view.vk_handle();

  /* device_format_get() returns Blender's TextureFormat.
   * to_vk_format() maps it to the corresponding VkFormat via the internal table. */
  const VkFormat vk_fmt = gpu::to_vk_format(vk_tex.device_format_get());

  /* VkImage and VkImageView are non-dispatchable handles defined as uint64_t
   * on all platforms (VK_DEFINE_NON_DISPATCHABLE_HANDLE). The cast is lossless. */
  result.vk_image      = uint64_t(vk_image);
  result.vk_image_view = uint64_t(vk_view);
  result.vk_format     = uint32_t(vk_fmt);

  return result;
}

/* ------------------------------------------------------------------ */

GPUVKDeviceHandles GPU_vk_device_handles_get()
{
  GPUVKDeviceHandles result = {};

  if (GPU_backend_get_type() != GPU_BACKEND_VULKAN) {
    return result;
  }

  /* VKBackend::get() returns the singleton VKBackend.
   * .device is the public VKDevice member declared in vk_backend.hh. */
  const gpu::VKDevice &dev = gpu::VKBackend::get().device;

  /* VkPhysicalDevice and VkDevice are dispatchable handles (pointers).
   * We store them as uint64_t via uintptr_t to keep Vulkan types out of
   * the GPU_texture.hh public header. */
  result.vk_physical_device = uint64_t(
      reinterpret_cast<uintptr_t>(dev.physical_device_get()));
  result.vk_device = uint64_t(
      reinterpret_cast<uintptr_t>(dev.vk_handle()));

  return result;
}

} // namespace blender
