/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_defines.hh"
#include "GPU_texture.hh"

namespace blender::eevee_game {

class Film {
public:
  void init(const int2 &display_extent);
  void sync();
  
  // High-speed blit to final window
  void present(gpu::Texture *final_color_tx);

  // Getters for internal logic
  int2 render_extent_get() const { return render_extent_; }
  int2 display_extent_get() const { return display_extent_; }
  float2 get_pixel_jitter() const { return jitter_; }
  
private:
  int2 display_extent_;
  int2 render_extent_;
  float2 jitter_;
};

} // namespace blender::eevee_game
