/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_bloom.hh"
#include "eevee_game_instance.hh"

namespace blender::eevee_game {

BloomModule::BloomModule(GameInstance &inst) : inst_(&inst) {}

void BloomModule::init()
{
  /* Standard AAA defaults. */
  settings_.threshold = 1.0f;
  settings_.intensity = 0.04f;
}

void BloomModule::sync()
{
  int2 res = inst_->film.render_extent_get();

  /* ensure_2d() is a no-op when size and format are unchanged, so this loop
   * only touches GPU memory on the first frame or after a resolution change. */
  for (int i = 0; i < PYRAMID_LEVELS; ++i) {
    res = math::max(int2(1), res / 2);
    bloom_pyramid_[i].ensure_2d(gpu::TextureFormat::RGBA16F, res,
                                GPU_TEXTURE_USAGE_SHADER_WRITE |
                                GPU_TEXTURE_USAGE_SHADER_READ);
  }

  bloom_downsample_ps_.init();
  bloom_downsample_ps_.shader_set(inst_->shaders.static_shader_get(SH_BLOOM_DOWNSAMPLE));

  bloom_upsample_ps_.init();
  bloom_upsample_ps_.shader_set(inst_->shaders.static_shader_get(SH_BLOOM_UPSAMPLE));

  /* Composite pass: reads bloom_pyramid_[0] and adds it onto combined_tx in-place.
   * Using READ_WRITE image access so the shader can: out = in_combined + bloom * intensity. */
  bloom_composite_ps_.init();
  bloom_composite_ps_.shader_set(inst_->shaders.static_shader_get(SH_BLOOM_COMPOSITE));
}

void BloomModule::render(gpu::Texture *input_color_tx)
{
  if (!settings_.enabled) {
    return;
  }

  GPU_debug_group_begin("Bloom");

  /* --- Phase 1: Downsampling (building the Mip Pyramid) ---
   *
   * Level 0 applies the luminance threshold and knee curve (soft-knee Hermite).
   * Levels 1..N are plain 2x downsamples using a 13-tap Kawase filter,
   * which suppresses the "firefly" aliasing that a naive box filter produces. */
  gpu::Texture *current_src = input_color_tx;
  for (int i = 0; i < PYRAMID_LEVELS; ++i) {
    bloom_downsample_ps_.bind_texture("in_color_tx", current_src);
    bloom_downsample_ps_.bind_image("out_color_img", &bloom_pyramid_[i]);

    /* Level 0: encode threshold + knee into params.xy; higher levels: params = -1 (bypass). */
    float4 params = (i == 0) ?
                    float4(settings_.threshold, settings_.knee, 0.0f, 0.0f) :
                    float4(-1.0f);
    bloom_downsample_ps_.push_constant("params", params);

    bloom_downsample_ps_.dispatch(
        math::divide_ceil(bloom_pyramid_[i]->size().xy(), int2(8)));
    bloom_downsample_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS |
                                  GPU_BARRIER_TEXTURE_FETCH);

    current_src = &bloom_pyramid_[i];
  }

  /* --- Phase 2: Upsampling (Blur + Accumulation) ---
   *
   * Iterates from the coarsest level back to level 0, additively blending
   * each level into the one above it.  The tent-filter upsample provides
   * smooth spatial falloff without ringing. */
  for (int i = PYRAMID_LEVELS - 1; i > 0; --i) {
    bloom_upsample_ps_.bind_texture("in_blur_tx",  &bloom_pyramid_[i]);
    bloom_upsample_ps_.bind_image("out_color_img", &bloom_pyramid_[i - 1]);
    bloom_upsample_ps_.push_constant("radius", settings_.radius);

    bloom_upsample_ps_.dispatch(
        math::divide_ceil(bloom_pyramid_[i - 1]->size().xy(), int2(8)));
    bloom_upsample_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS |
                                GPU_BARRIER_TEXTURE_FETCH);
  }

  GPU_debug_group_end();
}

void BloomModule::composite(gpu::Texture *combined_tx)
{
  /* FIX: This method did not exist before.  Previously render() built the pyramid
   * but the result was never applied to the scene color buffer.
   *
   * Additive composite: for each pixel,
   *   combined_tx[p] += bloom_pyramid_[0][p] * intensity
   *
   * We use a compute shader with READ_WRITE image binding so the hardware does
   * a single pass (read → add → write) with no extra temporary allocation.
   * A framebuffer blend approach would require a separate resolve texture and
   * an extra full-screen draw call — strictly worse for bandwidth. */
  if (!settings_.enabled) {
    return;
  }

  GPU_debug_group_begin("Bloom.Composite");

  /* bloom_pyramid_[0] is at render resolution / 2.  The composite shader samples
   * it with bilinear filtering to produce the full-resolution contribution. */
  bloom_composite_ps_.bind_texture("bloom_tx",    &bloom_pyramid_[0]);
  bloom_composite_ps_.bind_image("combined_img",  combined_tx);
  bloom_composite_ps_.push_constant("bloom_intensity", settings_.intensity);

  const int2 full_res = combined_tx->size().xy();
  bloom_composite_ps_.dispatch(math::divide_ceil(full_res, int2(8)));
  bloom_composite_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS |
                               GPU_BARRIER_TEXTURE_FETCH);

  GPU_debug_group_end();
}

} // namespace blender::eevee_game
