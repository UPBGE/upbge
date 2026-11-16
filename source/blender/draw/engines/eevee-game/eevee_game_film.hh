/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_defines.hh"
#include "GPU_texture.hh"

namespace blender::eevee_game {

class Film {
public:
  Film(class GameInstance &inst) : inst_(inst) {}

  void init(const int2 &display_extent);
  void sync();

  /* Draw the finished frame into the viewport color buffer. */
  void present(gpu::Texture *final_color_tx);

  // Getters for internal logic
  int2 render_extent_get() const { return render_extent_; }
  int2 display_extent_get() const { return display_extent_; }
  float2 get_pixel_jitter() const { return jitter_; }

private:
  class GameInstance &inst_;

  int2 display_extent_ = int2(1);
  int2 render_extent_ = int2(1);
  float2 jitter_ = float2(0.0f);

  /* Bound by reference: the source changes with the upscale / AA mode. */
  gpu::Texture *present_tx_ = nullptr;
  PassSimple present_ps_{"Film.Present"};
};

} // namespace blender::eevee_game
