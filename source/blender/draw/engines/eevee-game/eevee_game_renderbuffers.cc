/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_renderbuffers.hh"
#include "eevee_game_instance.hh"

namespace blender::eevee_game {

bool RenderBuffers::acquire(int2 extent)
{
  const eGPUTextureUsage usage_rw = GPU_TEXTURE_USAGE_SHADER_READ |
                                    GPU_TEXTURE_USAGE_SHADER_WRITE | GPU_TEXTURE_USAGE_ATTACHMENT;
  const eGPUTextureUsage usage_depth = GPU_TEXTURE_USAGE_SHADER_READ |
                                       GPU_TEXTURE_USAGE_ATTACHMENT;

  /* Called once per frame before anything references the targets: what was
   * combined_tx last frame becomes this frame's history. */
  Texture::swap(combined_tx, combined_prev_tx);

  depth_tx.ensure_2d(gpu::TextureFormat::SFLOAT_32_DEPTH_UINT_8, extent, usage_depth);
  const bool reallocated = combined_tx.ensure_2d(
      gpu::TextureFormat::SFLOAT_16_16_16_16, extent, usage_rw);
  vector_tx.ensure_2d(gpu::TextureFormat::SFLOAT_16_16, extent, usage_rw);

  combined_prev_tx.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, extent, usage_rw);

  const eGPUTextureUsage usage_image = GPU_TEXTURE_USAGE_SHADER_READ |
                                       GPU_TEXTURE_USAGE_SHADER_WRITE;
  rp_color_tx.ensure_2d_array(gpu::TextureFormat::SFLOAT_16_16_16_16, int2(1), 1, usage_image);
  rp_value_tx.ensure_2d_array(gpu::TextureFormat::SFLOAT_16, int2(1), 1, usage_image);
  cryptomatte_tx.ensure_2d(gpu::TextureFormat::SFLOAT_32_32_32_32, int2(1), usage_image);

  shadow_mask_tx.ensure_2d(gpu::TextureFormat::SFLOAT_16, extent, usage_rw);

  reactive_mask_tx.ensure_2d(gpu::TextureFormat::UNORM_8, extent, usage_rw);
  transp_mask_tx.ensure_2d(gpu::TextureFormat::UNORM_8, extent, usage_rw);

  /* The HUD is never upscaled. */
  ui_color_tx.ensure_2d(
      gpu::TextureFormat::UNORM_8_8_8_8, inst_.film.display_extent_get(), usage_rw);

  return reallocated;
}

} // namespace blender::eevee_game
