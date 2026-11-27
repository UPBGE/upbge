/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_defines.hh"
#include "GPU_texture.hh"

namespace blender::eevee_game {

/* Largest CoC radius in pixels the 16 sample gather covers without visible gaps. */
#define DOF_FAST_MAX_RADIUS 16.0f

struct DOFSettings {
  /* Scene units, from the camera focus distance or focus object. */
  float focus_distance = 10.0f;
  /* CoC radius in pixels = coc_scale * (1 / focus_distance - 1 / view_distance). */
  float coc_scale = 0.0f;
  /* Pixels: scene bokeh max size, capped by DOF_FAST_MAX_RADIUS. */
  float max_bokeh_radius = 8.0f;
  bool enabled = false;
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