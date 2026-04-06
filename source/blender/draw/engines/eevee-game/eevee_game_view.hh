/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_defines.hh"
#include "draw_pass.hh"
#include "GPU_texture.hh"
#include "GPU_framebuffer.hh"

namespace blender::eevee_game {

/**
 * ShadingView represents a single point of view (Main Camera).
 * It orchestrates the high-level sequence of rendering passes.
 */
class ShadingView {
 public:
  ShadingView(class GameInstance &inst) : inst_(inst) {}

  void init(const char *name);

  /* Main entry point: runs the full frame pipeline in order. */
  void render();

 private:
  /* Updates camera matrices and applies sub-pixel jitter for FSR temporal accumulation. */
  void update_view();

  /* Depth + velocity prepass — writes depth_tx and vector_tx before G-Buffer fill. */
  void render_prepass();

  const char *name_  = nullptr;
  int2        extent_ = int2(0); /* Render resolution (may be less than display when FSR is ON). */

  /* Primary and render views.
   * main_view_   — includes sub-pixel jitter for FSR.
   * render_view_ — matches the actual draw resolution, used for culling and rasterisation. */
  View main_view_{"ShadingView.Main"};
  View render_view_{"ShadingView.Render"};

  /* Framebuffers.
   * combined_fb_ : color attachment = render_buffers.combined_tx (HDR RGBA16F).
   * prepass_fb_  : depth-only, writes render_buffers.depth_tx.
   * gbuffer_fb_  : MRT — depth (read-only) + G-Buffer color layers. */
  Framebuffer combined_fb_{"Combined"};
  Framebuffer gbuffer_fb_{"GBuffer"};
  Framebuffer prepass_fb_{"Prepass"};

  /* Post-processing intermediate textures.
   *
   * postfx_tx_   : DoF output (render resolution, RGBA16F).
   *                Input = combined_tx (after bloom), output = this.
   *                Must be distinct from combined_tx to avoid compute read/write aliasing.
   *
   * display_res_tx_ : FSR3 output (display resolution, RGBA16F).
   *                   Written by UpscaleModule::apply_fsr3().
   *
   * aa_out_tx_   : FXAA/SMAA output when FSR is OFF (render resolution).
   *               Separate from postfx_tx_ to keep SMAA's multi-pass chain
   *               source-distinct from the destination on each pass. */
  gpu::Texture postfx_tx_{"postfx"};
  gpu::Texture display_res_tx_{"display_res"};
  gpu::Texture aa_out_tx_{"aa_out"};

  class GameInstance &inst_;

  friend class GameInstance;
};

} // namespace blender::eevee_game
