/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_renderbuffers.hh"
#include "eevee_game_instance.hh"

namespace blender::eevee_game {

void RenderBuffers::init() {
  const eViewLayerEEVEEPassType enabled_passes = inst_.film.enabled_passes_get();
  data.color_len = 0;
  data.value_len = 0;

  // Map only the necessary gaming passes to the G-Buffer
  auto pass_index_get = [&](eViewLayerEEVEEPassType pass_type) {
    if (enabled_passes & pass_type) {
      return (pass_storage_type(pass_type) == PASS_STORAGE_COLOR) ? data.color_len++ : data.value_len++;
    }
    return -1;
  };

  data.normal_id = pass_index_get(EEVEE_RENDER_PASS_NORMAL);
  data.diffuse_light_id = pass_index_get(EEVEE_RENDER_PASS_DIFFUSE_LIGHT);
  data.specular_light_id = pass_index_get(EEVEE_RENDER_PASS_SPECULAR_LIGHT);
  data.ambient_occlusion_id = pass_index_get(EEVEE_RENDER_PASS_AO);
}

void RenderBuffers::acquire(int2 extent) {
  extent_ = extent;
  eGPUTextureUsage usage_rw = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE | 
                               GPU_TEXTURE_USAGE_ATTACHMENT;

  // 1. Core Buffers (Depth in 32-bit for precision, Color in 16-bit for performance)
  depth_tx.ensure_2d(gpu::TextureFormat::SFLOAT_32_DEPTH_UINT_8, extent, usage_rw);
  combined_tx.acquire(extent, gpu::TextureFormat::SFLOAT_16_16_16_16);
  vector_tx.acquire(extent, gpu::TextureFormat::SFLOAT_16_16, usage_rw);

  // 2. G-Buffer Layers (SFLOAT_16 optimized for Closures)
  int color_len = math::max(1, data.color_len);
  int value_len = math::max(1, data.value_len);
  rp_color_tx.ensure_2d_array(gpu::TextureFormat::SFLOAT_16_16_16_16, extent, color_len, usage_rw);
  rp_value_tx.ensure_2d_array(gpu::TextureFormat::SFLOAT_16, extent, value_len, usage_rw);

  /* Shadow mask: written exclusively by the PCF compute pass, read by deferred lighting.
   * Allocated at render resolution (same as depth/normal).
   * R16F — 2 bytes per pixel, ~4 MB at 1440p. No pooling needed: persists whole frame. */
  shadow_mask_tx.ensure_2d(gpu::TextureFormat::SFLOAT_16, extent, usage_rw);

  // 3. FSR 3.0 Specific Buffers
  // Masks are generated at render resolution
  reactive_mask_tx.acquire(extent, gpu::TextureFormat::R8, usage_rw);
  transp_mask_tx.acquire(extent, gpu::TextureFormat::R8, usage_rw);
  
  // UI Buffer is at full display resolution (no upscale for HUD)
  int2 display_res = inst_.film.display_extent_get();
  ui_color_tx.acquire(display_res, gpu::TextureFormat::RGBA8, usage_rw);
}

void RenderBuffers::release() {
  combined_tx.release();
  vector_tx.release();
  reactive_mask_tx.release();
  transp_mask_tx.release();
  ui_color_tx.release();
  /* shadow_mask_tx is ensure_2d (not pooled), so no release needed —
   * it persists across frames and is reused in-place each frame. */
}

} // namespace blender::eevee_game
