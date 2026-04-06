/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_view.hh"
#include "eevee_game_instance.hh"

namespace blender::eevee_game {

/* ================================================================
 * sync()
 *
 * Allocates all resolution-dependent textures used by the post-processing
 * chain. Must be called:
 *   - Once during GameInstance::init() before the first frame.
 *   - Whenever the render or display resolution changes (window resize,
 *     FSR mode change).
 *
 * FIX: postfx_tx_, aa_out_tx_, display_res_tx_ were gpu::Texture members
 * that were never given a GPU handle via ensure_2d(). Any pass that tried
 * to bind them would receive a null texture → GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT
 * or a driver-level null dereference.
 * ================================================================ */

void ShadingView::sync()
{
  const int2 render_res  = inst_.film.render_extent_get();
  const int2 display_res = inst_.film.display_extent_get();

  /* Usage flags for intermediate post-processing targets:
   *   SHADER_READ  — sampled as a texture by the next pass in the chain.
   *   SHADER_WRITE — written by compute imageStore.
   *   ATTACHMENT   — bound as a framebuffer colour attachment (SMAA raster passes). */
  constexpr eGPUTextureUsage postfx_usage = GPU_TEXTURE_USAGE_SHADER_READ  |
                                            GPU_TEXTURE_USAGE_SHADER_WRITE |
                                            GPU_TEXTURE_USAGE_ATTACHMENT;

  /* postfx_tx_: DoF writes here at render resolution.
   * Input = combined_tx (after bloom composite); distinct to avoid compute aliasing. */
  postfx_tx_.ensure_2d(gpu::TextureFormat::RGBA16F, render_res, postfx_usage);

  /* aa_out_tx_: FXAA/SMAA writes here at render resolution.
   * Separate from postfx_tx_ so SMAA's 3-pass chain has a stable source for each pass. */
  aa_out_tx_.ensure_2d(gpu::TextureFormat::RGBA16F, render_res, postfx_usage);

  /* display_res_tx_: FSR3 upscale output at DISPLAY resolution.
   * Allocated at display_res (not render_res) — this is the larger of the two when
   * FSR is active. When FSR is OFF this equals render_res. */
  display_res_tx_.ensure_2d(gpu::TextureFormat::RGBA16F, display_res, postfx_usage);

  /* Bind combined_tx to its framebuffer so the deferred lighting pass can write to it.
   * The texture itself is owned by RenderBuffers and already allocated there. */
  GPU_framebuffer_ensure_config(
      &combined_fb_,
      {
          GPU_ATTACHMENT_NONE,
          GPU_ATTACHMENT_TEXTURE(inst_.render_buffers.combined_tx.get()),
      });
}

/* ================================================================
 * render()
 * ================================================================ */

void ShadingView::render()
{
  RenderBuffers &rbufs = inst_.render_buffers;

  /* ================================================================
   * Step 1: GPU-Driven Culling
   * Eliminates non-visible instances before any rasterisation work. */
  inst_.culling.execute_culling(render_view_);

  /* ================================================================
   * Step 2: Depth/Velocity Prepass
   * Populates depth_tx for EQUAL depth testing and vector_tx for FSR3. */
  render_prepass();

  /* Hi-Z update: downsample depth_tx into a mip chain used by SSR/SSGI and
   * next-frame occlusion culling. */
  inst_.hiz_update_ps_.bind_texture("depth_tx", &rbufs.depth_tx);
  inst_.hiz_update_ps_.dispatch(
      math::divide_ceil(inst_.film.render_extent_get(), int2(8)));

  /* ================================================================
   * Step 3: Main Rendering Pipeline
   * G-Buffer → GTAO → SSGI → Tiled Deferred Lighting → Transparent Forward.
   * FIX: was called as pipelines.render(render_view_) — missing combined_fb_ argument. */
  inst_.pipelines.render(render_view_, combined_fb_);

  /* ================================================================
   * Step 4: Post-Processing chain
   *
   * combined_tx  ──Bloom additive──► combined_tx  ──DoF──► postfx_tx_
   *                                                              │
   *                                                    AA or FSR3 below
   *
   * All three textures (combined_tx, postfx_tx_, aa_out_tx_/display_res_tx_)
   * are distinct — necessary to avoid compute shader read/write aliasing. */

  /* Bloom: build the downsample/upsample pyramid, then additively composite
   * pyramid[0] back onto combined_tx in a single READ_WRITE compute pass. */
  inst_.bloom.render(rbufs.combined_tx.get());
  inst_.bloom.composite(rbufs.combined_tx.get());

  /* DoF: reads combined_tx, writes postfx_tx_.
   * postfx_tx_ was allocated in sync() — always has a valid GPU handle here. */
  inst_.dof.render_fast(rbufs.combined_tx.get(), &postfx_tx_);

  /* ================================================================
   * Step 5: Upscaling / Anti-Aliasing */
  gpu::Texture *final_output = nullptr;

  if (inst_.upscale_settings.mode != UpscaleMode::OFF) {
    /* FSR3: reads postfx_tx_ (render_res), writes display_res_tx_ (display_res).
     * display_res_tx_ allocated at display resolution in sync(). */
    inst_.upscale.apply_fsr3(&postfx_tx_, &display_res_tx_, nullptr);
    final_output = &display_res_tx_;
  }
  else {
    /* Spatial AA: reads postfx_tx_, writes aa_out_tx_.
     * Both textures have valid GPU handles from sync(). */
    inst_.pipelines.aa.apply_aa(&postfx_tx_, &aa_out_tx_,
                                 inst_.pipelines.aa_settings);
    final_output = &aa_out_tx_;
  }

  /* ================================================================
   * Step 6: Present */
  inst_.film.present(final_output);
}

} // namespace blender::eevee_game
