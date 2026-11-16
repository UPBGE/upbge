/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_culling.hh"
#include "eevee_game_defines.hh"
#include "draw_pass.hh"
#include "GPU_texture.hh"
#include "GPU_framebuffer.hh"

namespace blender::eevee_game {

/**
 * ShadingView represents a single point of view (Main Camera).
 * It orchestrates the high-level sequence of rendering passes for one frame.
 *
 * Lifetime:
 *   init()  — called once at engine startup; acquires persistent GPU resources.
 *   sync()  — called each frame before render(); reallocates textures if resolution changed.
 *   render()— called each frame; submits GPU work.
 */
class ShadingView {
 public:
  /* `render_view_` is declared before `inst_`, so it takes the constructor
   * argument and not the member. */
  ShadingView(class GameInstance &inst) : render_view_("ShadingView.Render", inst), inst_(inst) {}

  /* Acquire per-view GPU resources that do not depend on resolution.
   * Called once from GameInstance constructor. */
  void init(const char *name);

  /* Reallocate resolution-dependent textures and rebuild framebuffer configs.
   * Must be called before the first render() and whenever the resolution changes
   * (FSR mode change, window resize).
   *
   * FIX: postfx_tx_, aa_out_tx_, display_res_tx_ were never allocated before this
   * method was added. Accessing them in render() would produce
   * GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT or a null GPU handle crash. */
  void sync();

  /* Execute the full frame pipeline:
   *   Prepass (culled on first submit) → HiZ → Pipeline → Bloom → DoF → AA/FSR → Present */
  void render();

  /* Returns the texture that holds the completed frame after render().
   * Used by GameInstance::render_frame() for F12 pixel readback. */
  gpu::Texture *get_final_texture();

 private:
  void update_view();
  void render_prepass();

  const char *name_  = nullptr;
  int2        extent_ = int2(0);

  View main_view_{"ShadingView.Main"};
  /* Frustum + Hi-Z occlusion culling happens inside this view. */
  GameView render_view_;

  Framebuffer combined_fb_{"Combined"};
  Framebuffer gbuffer_fb_{"GBuffer"};
  Framebuffer prepass_fb_{"Prepass"};

  /* Post-processing textures — allocated in sync(), never accessed before it.
   *
   * postfx_tx_     : DoF output at render resolution (RGBA16F).
   *                  Must be distinct from combined_tx to avoid compute read/write aliasing.
   *
   * display_res_tx_: FSR3 output at display resolution (RGBA16F).
   *                  Larger than render resolution when FSR is active.
   *                  Allocated at display_res, not render_res.
   *
   * aa_out_tx_     : FXAA/SMAA output at render resolution (RGBA16F).
   *                  Separate from postfx_tx_ to keep the SMAA multi-pass
   *                  source distinct from the destination on each pass. */
  Texture postfx_tx_{"postfx"};
  Texture display_res_tx_{"display_res"};
  Texture aa_out_tx_{"aa_out"};

  /* Whichever texture ended up holding the finished frame: FSR output, AA
   * output, or the last post-processing target when both are off. */
  gpu::Texture *final_tx_ = nullptr;

  class GameInstance &inst_;

  friend class GameInstance;
};

} // namespace blender::eevee_game
