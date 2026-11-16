/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_defines.hh"
#include "GPU_texture.hh"

namespace blender::eevee_game {

struct DOFSettings {
  float focus_distance = 10.0f;
  float focal_length = 50.0f;
  float f_stop = 2.8f;
  float sensor_size = 36.0f; // mm
  float max_bokeh_radius = 8.0f; // Screen space pixels
  bool enabled = true;
};

/**
 * Depth of Field module optimized for real-time games.
 * Uses a half-res bokeh blur with bilateral reconstruction.
 */
class DepthOfField {
 public:
  DepthOfField(class GameInstance &inst);
  
  void init();
  void sync();
  
  /**
   * Main rendering entry point for ShadingView::render_postfx.
   * @param input_color_tx The HDR radiance buffer.
   * @param output_color_tx The target buffer for the blurred result.
   */
  void render_fast(gpu::Texture *input_color_tx, gpu::Texture *output_color_tx);

  /* When false render_fast() writes nothing and the caller must keep using
   * its input texture. */
  bool enabled() const { return settings_.enabled; }

 private:
  GameInstance *inst_;
  DOFSettings settings_;

  // Half-resolution textures for performance
  Texture coc_tx_ = {"coc_tx_"};        // Circle of Confusion buffer
  Texture bokeh_half_tx_ = {"bokeh_half_tx_"}; // Blurred color buffer

  // Pass objects
  PassSimple coc_setup_ps_{"DOF.CoCSetup"};
  PassSimple bokeh_blur_ps_{"DOF.BokehBlur"};
  PassSimple dof_resolve_ps_{"DOF.Resolve"};

  friend class GameInstance;
};

} // namespace blender::eevee_game