/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Dispatcher for Vulkan external-memory texture import.
 * Supports DMA-BUF (Linux) and D3D11 shared handle (Windows).
 * All real work is in VKTexture::init_internal_external_memory().
 */

#include "GPU_texture.hh"
#include "gpu_texture_private.hh"

#ifdef WITH_VULKAN_BACKEND
#  include "vulkan/vk_texture.hh"
#endif

namespace blender {

#ifdef WITH_VULKAN_BACKEND

gpu::Texture *GPU_texture_create_from_external_memory(const char *name,
                                                      int w,
                                                      int h,
                                                      uint32_t stride,
                                                      gpu::TextureFormat format,
                                                      uint32_t handle_type,
                                                      int64_t handle)
{
  gpu::Texture *tex = GPU_texture_create_2d(
      name, w, h, 1, format, GPU_TEXTURE_USAGE_SHADER_READ, nullptr);
  if (tex == nullptr) {
    return nullptr;
  }
  auto *vk_tex = static_cast<gpu::VKTexture *>(tex);
  if (!vk_tex->init_internal_external_memory(
          VkExternalMemoryHandleTypeFlagBits(handle_type), handle, stride))
  {
    GPU_texture_free(tex);
    return nullptr;
  }
  return tex;
}

#endif /* WITH_VULKAN_BACKEND */

}  // namespace blender
