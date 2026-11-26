/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_defines.hh"
#include "GPU_texture.hh"

namespace blender::eevee_game {

/**
 * Bloom of legacy EEVEE (Blender 2.80 to 4.1), driven by the scene's bloom settings: a blit to
 * half resolution that keeps what is above the threshold, a chain of downsamples whose length
 * follows the radius, the chain added back up with a tent filter, and the result added to the
 * scene color times the bloom color.
 */
class BloomModule {
 public:
  BloomModule(class GameInstance &inst);

  void init() {}
  /* Reads the scene settings and sizes the chain for the render resolution. */
  void sync();

  bool enabled() const
  {
    return enabled_;
  }

  /* Builds the chain from the HDR scene color. Not modified by this call. */
  void render(gpu::Texture *input_color_tx);

  /* Adds the bloom to combined_tx in place. After render(), before the passes that read
   * combined_tx (DoF, upscaling, anti-aliasing). */
  void composite(gpu::Texture *combined_tx);

 private:
  GameInstance *inst_;

  /* Legacy MAX_BLOOM_STEP. */
  static constexpr int STEP_MAX = 16;

  bool enabled_ = false;
  int step_count_ = 0;
  float sample_scale_ = 1.0f;
  float4 curve_threshold_ = float4(0.0f);
  float clamp_ = 0.0f;
  float3 color_ = float3(0.0f);

  /* Half resolution blit, then each level half the previous one. */
  Texture blit_tx_ = {"bloom_blit"};
  Texture step_tx_[STEP_MAX];

  PassSimple downsample_ps_{"Bloom.Downsample"};
  PassSimple upsample_ps_{"Bloom.Upsample"};
  PassSimple composite_ps_{"Bloom.Composite"};
};

}  // namespace blender::eevee_game
