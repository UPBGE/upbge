/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_view.hh"
#include "eevee_game_instance.hh"

namespace blender::eevee_game {

void ShadingView::render()
{
  /* Aliases used throughout: render_buffers lives on GameInstance. */
  RenderBuffers &rbufs = inst_.render_buffers;

  /* ================================================================
   * Step 1: GPU-Driven Culling
   * Eliminates non-visible instances before any rasterisation work.
   * The compute dispatch writes a compact visible_indices_buf that the
   * indirect draw calls consume. */
  inst_.culling.execute_culling(render_view_);

  /* ================================================================
   * Step 2: Depth/Velocity Prepass
   * Populates depth_tx for EQUAL depth testing in the G-Buffer pass.
   * Also writes motion vectors into vector_tx for FSR3 and Hi-Z. */
  render_prepass();

  /* Hi-Z update: downsample depth_tx into a mip chain used by:
   *   - SSR/SSGI screen-space ray marching (coarse early-out)
   *   - Culling occlusion test for the next frame */
  inst_.hiz_update_ps_.dispatch(
      math::divide_ceil(inst_.film.render_extent_get(), int2(8)));

  /* ================================================================
   * Step 3: Main Rendering Pipeline
   * G-Buffer fill → GTAO → SSGI → Tiled Deferred Lighting → Transparent forward.
   * FIX: pass combined_fb_ as the second argument (was missing, causing compile error).
   * combined_fb_ is the framebuffer whose color attachment IS rbufs.combined_tx. */
  inst_.pipelines.render(render_view_, combined_fb_);

  /* ================================================================
   * Step 4: Post-Processing chain at render resolution.
   *
   * The chain reads from combined_tx and either modifies it in-place (Bloom additive)
   * or ping-pongs through postfx_tx_ (DoF).
   *
   * Order: Bloom → DoF → (AA / FSR below)
   *
   * combined_tx  ──Bloom additive──►  combined_tx  ──DoF──►  postfx_tx_
   *
   * Using combined_tx as both DoF input and output would alias reads/writes —
   * that is undefined behaviour in a compute shader.  postfx_tx_ is the safe target. */

  /* --- Bloom ---
   * BloomModule::render() builds the downsample→upsample pyramid in bloom_pyramid_[].
   * The result lives in bloom_pyramid_[0].
   * We then composite it additively back onto combined_tx via a separate compute pass
   * so the full-resolution HDR buffer already has bloom baked in before DoF reads it.
   *
   * FIX: previously bloom.render() was called but its result was never consumed.
   * Now a dedicated composite pass adds pyramid[0] * intensity onto combined_tx. */
  inst_.bloom.render(rbufs.combined_tx.get());
  inst_.bloom.composite(rbufs.combined_tx.get()); /* additive blend pyramid[0] → combined_tx */

  /* --- Depth of Field ---
   * render_fast() reads input_color_tx and writes to output_color_tx.
   * FIX: was called as dof.render_fast(combined_tx) with only one argument — the
   * signature requires an explicit output target to avoid read/write aliasing.
   * postfx_tx_ is guaranteed distinct from combined_tx. */
  inst_.dof.render_fast(rbufs.combined_tx.get(), &postfx_tx_);

  /* After DoF: postfx_tx_ holds the final post-processed image at render resolution.
   * All subsequent passes read from postfx_tx_. */
  gpu::Texture *postfx_result = &postfx_tx_;

  /* ================================================================
   * Step 5: Upscaling / Anti-Aliasing
   *
   * Path A — FSR3 temporal upscaling (render_res → display_res)
   * Path B — Spatial AA (FXAA or SMAA, stays at render_res) */
  gpu::Texture *final_output = nullptr;

  if (inst_.upscale_settings.mode != UpscaleMode::OFF) {
    /* FSR3 reads postfx_result (render_res) and writes display_res_tx_ (display_res).
     * ui_tx may be null in game mode with no HUD overlay. */
    inst_.upscale.apply_fsr3(postfx_result, &display_res_tx_, nullptr);
    final_output = &display_res_tx_;
  }
  else {
    /* Spatial AA reads postfx_result, writes aa_out_tx_ (same resolution).
     * We use a dedicated aa_out_tx_ instead of writing back to postfx_result to
     * keep the source/destination distinct for the SMAA multi-pass chain. */
    inst_.pipelines.aa.apply_aa(postfx_result, &aa_out_tx_,
                                 inst_.pipelines.aa_settings);
    final_output = &aa_out_tx_;
  }

  /* ================================================================
   * Step 6: Present to screen
   * Film::present() blits final_output to the viewport default framebuffer. */
  inst_.film.present(final_output);
}

} // namespace blender::eevee_game
