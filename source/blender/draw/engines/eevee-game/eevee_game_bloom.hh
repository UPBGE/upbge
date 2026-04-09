/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_defines.hh"
#include "GPU_texture.hh"

namespace blender::eevee_game {

struct BloomSettings {
  float threshold = 1.0f;   /* Only pixels brighter than this produce bloom. */
  float knee      = 0.1f;   /* Soft-knee width: smooths the hard threshold cut. */
  float intensity = 0.04f;  /* Additive blend weight applied in composite(). */
  float radius    = 0.85f;  /* Tent-filter radius during the upsample pass. */
  bool  enabled   = true;
};

class BloomModule {
 public:
  BloomModule(class GameInstance &inst);

  void init();
  void sync();

  /**
   * Build the downsample → upsample luminance pyramid.
   * After this call, bloom_pyramid_[0] holds the full-resolution bloom contribution.
   * @param input_color_tx  HDR scene color (RGBA16F).  Not modified by this call.
   */
  void render(gpu::Texture *input_color_tx);

  /**
   * Additively blend bloom_pyramid_[0] onto combined_tx in-place.
   * FIX: this method was missing — without it the bloom pyramid was computed
   * but never applied, so bloom had no visible effect.
   *
   * Must be called after render() and before any pass that reads combined_tx
   * (DoF, FSR3, AA).
   *
   * @param combined_tx  HDR framebuffer to receive the bloom (READ_WRITE image).
   */
  void composite(gpu::Texture *combined_tx);

  /* Return the base of the pyramid (render_res / 2, RGBA16F).
   * Exposed for debugging / RenderDoc inspection. */
  gpu::Texture *get_result() { return &bloom_pyramid_[0]; }

 private:
  GameInstance *inst_;
  BloomSettings settings_;

  /* Mip pyramid: bloom_pyramid_[0] = render_res/2, [PYRAMID_LEVELS-1] = coarsest.
   * Each level is half the width and height of the previous. */
  static constexpr int PYRAMID_LEVELS = 6;
  /* Plain members so ensure_2d() can skip re-allocation when the resolution
   * is unchanged — which is every frame at steady state. The previous
   * make_unique<> pattern destroyed and recreated all six RGBA16F textures
   * on every sync() call, producing VRAM thrash and frame-time spikes. */
  gpu::Texture bloom_pyramid_[PYRAMID_LEVELS];

  PassSimple bloom_downsample_ps_{"Bloom.Downsample"};
  PassSimple bloom_upsample_ps_{"Bloom.Upsample"};
  PassSimple bloom_composite_ps_{"Bloom.Composite"};
};

} // namespace blender::eevee_game
