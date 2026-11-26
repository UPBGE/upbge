/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_scene_types.h"

#include "eevee_game_bloom.hh"
#include "eevee_game_instance.hh"

namespace blender::eevee_game {

BloomModule::BloomModule(GameInstance &inst) : inst_(&inst) {}

void BloomModule::sync()
{
  const SceneEEVEE &eevee = inst_->scene->eevee;
  enabled_ = (eevee.flag & SCE_EEVEE_BLOOM_ENABLED) && eevee.bloom_intensity > 0.0f;
  if (!enabled_) {
    /* Nothing to keep for a scene that does not use it. */
    blit_tx_.free();
    for (Texture &tx : step_tx_) {
      tx.free();
    }
    return;
  }

  /* Legacy EEVEE's bloom_init(): the chain gets one step per halving of the smallest blit
   * dimension, minus 8 plus the radius, and the fraction left over widens the tent. */
  const int2 blit_res = math::max(int2(1), inst_->film.render_extent_get() / 2);
  const float steps = (eevee.bloom_radius - 8.0f) +
                      std::log2(float(math::min(blit_res.x, blit_res.y)));
  step_count_ = math::clamp(int(steps), 1, STEP_MAX);
  sample_scale_ = 0.5f + steps - float(int(steps));

  const float threshold = eevee.bloom_threshold;
  const float knee = threshold * eevee.bloom_knee + 1e-5f;
  curve_threshold_ = float4(threshold - knee, knee * 2.0f, 0.25f / knee, threshold);
  clamp_ = eevee.bloom_clamp;
  color_ = float3(eevee.bloom_color) * eevee.bloom_intensity;

  const eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_WRITE | GPU_TEXTURE_USAGE_SHADER_READ;
  blit_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, blit_res, usage);
  int2 res = blit_res;
  for (int i = 0; i < STEP_MAX; i++) {
    if (i < step_count_) {
      res = math::max(int2(2), res / 2);
      step_tx_[i].ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, res, usage);
    }
    else {
      step_tx_[i].free();
    }
  }
}

void BloomModule::render(gpu::Texture *input_color_tx)
{
  if (!enabled_) {
    return;
  }
  const GPUSamplerState linear = {GPU_SAMPLER_FILTERING_LINEAR};

  /* A pass is only a command list: every level is recorded, then submitted once. */
  downsample_ps_.init();
  downsample_ps_.shader_set(inst_->shaders.static_shader_get(SH_BLOOM_DOWNSAMPLE));
  downsample_ps_.push_constant("curve_threshold", curve_threshold_);
  downsample_ps_.push_constant("clamp_intensity", clamp_);
  gpu::Texture *src = input_color_tx;
  for (int i = -1; i < step_count_; i++) {
    Texture &dst = (i < 0) ? blit_tx_ : step_tx_[i];
    PassSimple::Sub &sub = downsample_ps_.sub("Level");
    sub.push_constant("is_blit", i < 0);
    sub.bind_texture("in_color_tx", src, linear);
    sub.bind_image("out_color_img", dst);
    sub.dispatch(math::divide_ceil(dst.size().xy(), int2(8)));
    sub.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
    src = dst;
  }
  inst_->manager->submit(downsample_ps_);

  /* Coarsest to finest, each level adding the tent filtered coarser one. */
  upsample_ps_.init();
  upsample_ps_.shader_set(inst_->shaders.static_shader_get(SH_BLOOM_UPSAMPLE));
  upsample_ps_.push_constant("sample_scale", sample_scale_);
  for (int i = step_count_ - 2; i >= 0; i--) {
    PassSimple::Sub &sub = upsample_ps_.sub("Level");
    sub.bind_texture("in_blur_tx", step_tx_[i + 1], linear);
    sub.bind_image("out_color_img", step_tx_[i]);
    sub.dispatch(math::divide_ceil(step_tx_[i].size().xy(), int2(8)));
    sub.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  }
  inst_->manager->submit(upsample_ps_);
}

void BloomModule::composite(gpu::Texture *combined_tx)
{
  if (!enabled_) {
    return;
  }
  const GPUSamplerState linear = {GPU_SAMPLER_FILTERING_LINEAR};
  composite_ps_.init();
  composite_ps_.shader_set(inst_->shaders.static_shader_get(SH_BLOOM_COMPOSITE));
  composite_ps_.bind_texture("bloom_tx", step_tx_[0], linear);
  composite_ps_.bind_image("combined_img", combined_tx);
  composite_ps_.push_constant("bloom_color", color_);
  composite_ps_.push_constant("sample_scale", sample_scale_);
  composite_ps_.dispatch(math::divide_ceil(texture_extent(combined_tx), int2(8)));
  composite_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  inst_->manager->submit(composite_ps_);
}

}  // namespace blender::eevee_game
