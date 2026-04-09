/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_view.hh"
#include "eevee_game_instance.hh"

namespace blender::eevee_game {

/* ================================================================
 * init()
 *
 * One-time setup of persistent GPU resources that do not depend on
 * resolution. Currently a no-op beyond storing the view name, but
 * reserved for future per-view GPU buffer allocation.
 * ================================================================ */

void ShadingView::init(const char *name)
{
  name_ = name;
}

/* ================================================================
 * update_view()
 *
 * Synchronises render_view_ with the current frame's camera matrices.
 * Must be called every frame before culling and pipeline submission,
 * because render_view_.persmat() is what CullingModule reads for the
 * frustum planes — using an identity matrix culls everything or nothing.
 *
 * We pass viewprojmat as persmat and viewinv as the inverse so the
 * View object can expose both the combined VP and the separate view
 * matrix to any pass that needs them.
 * ================================================================ */

void ShadingView::update_view()
{
  render_view_.sync(inst_.uniform_data.viewprojmat,
                    inst_.uniform_data.viewinv);
}

/* ================================================================
 * render_prepass()
 *
 * Depth + velocity prepass. Runs before the G-Buffer fill so the
 * G-Buffer shaders can use DEPTH_EQUAL (no depth write, free early-Z).
 *
 * Two sub-passes share the same framebuffer:
 *   - opaque_layer_.prepass_ps  : writes depth and optional velocity
 *   - forward_layer_.prepass_ps : transparent depth pre-write (for DoF CoC)
 *
 * The prepass framebuffer attaches depth_tx (write) and vector_tx
 * (write, for FSR3 motion vectors). combined_tx is not attached here —
 * it is bound in the G-Buffer framebuffer in PipelineModule::sync().
 * ================================================================ */

void ShadingView::render_prepass()
{
  RenderBuffers &rbufs = inst_.render_buffers;
  const int2 render_res = inst_.film.render_extent_get();

  /* Framebuffer: depth write + motion vector write.
   * vector_tx is RG16F (screen-space UV velocity per pixel).
   * No colour attachment for the opaque prepass — depth only. */
  GPU_framebuffer_ensure_config(
      &prepass_fb_,
      {
          GPU_ATTACHMENT_TEXTURE(&rbufs.depth_tx),
          GPU_ATTACHMENT_TEXTURE(rbufs.vector_tx.get()),
      });

  GPU_framebuffer_bind(&prepass_fb_);
  GPU_framebuffer_clear_depth(&prepass_fb_, 1.0f);

  /* Opaque depth + velocity — all materials submit here in sync_mesh().
   * DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS_EQUAL set per sub-pass
   * in PipelineModule::material_add(). */
  GPU_debug_group_begin("Prepass.Opaque");
  inst_.manager->submit(inst_.pipelines.opaque_layer_.prepass_ps, render_view_);
  GPU_debug_group_end();

  /* Transparent prepass: writes depth so DoF CoC is correct at glass edges.
   * Blended geometry depth is intentionally written here to prevent
   * CoC discontinuities at transparent-opaque boundaries. */
  GPU_debug_group_begin("Prepass.Transparent");
  inst_.manager->submit(inst_.pipelines.forward_layer_.prepass_ps, render_view_);
  GPU_debug_group_end();
}

/* ================================================================
 * sync()
 *
 * Allocates all resolution-dependent textures used by the post-processing
 * chain. Must be called:
 *   - Once during GameInstance::init() before the first frame.
 *   - Whenever the render or display resolution changes (window resize,
 *     FSR mode change).
 *
 * ================================================================ */

void ShadingView::sync()
{
  const int2 render_res  = inst_.film.render_extent_get();
  const int2 display_res = inst_.film.display_extent_get();

  /* Acquire all pooled render-buffer textures (combined_tx, vector_tx,
   * reactive_mask_tx, transp_mask_tx, ui_color_tx).
   * TextureFromPool::acquire() obtains a GPU handle from the DRW texture pool.
   * Without this call all five textures remain nullptr and every framebuffer
   * that references them is GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT.
   * release() is called at the end of render() to return them to the pool. */
  inst_.render_buffers.release(); /* no-op on first call; safe to call always */
  inst_.render_buffers.acquire(render_res);

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

  /* Sync render_view_ with the current frame's camera matrices.
   * Must precede culling — CullingModule reads render_view_.persmat()
   * for the Gribb-Hartmann frustum planes. */
  update_view();

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
  /* GPU_BARRIER_TEXTURE_FETCH: ensures imageStore writes from the HiZ mip
   * downsampling compute are visible to subsequent texture() reads in:
   *   - CullingModule::execute_culling()  (hiz_tx occlusion test, next frame)
   *   - GTAOModule::render()              (hiz_tx horizon search)
   *   - SSGIModule::render()              (hiz_tx ray march)
   *   - RayTraceModule::render()          (hiz_tx SSR trace)
   * Without this barrier all four shaders read stale or partially-written
   * mip data, producing flickering and incorrect occlusion culling results. */
  inst_.hiz_update_ps_.barrier(GPU_BARRIER_TEXTURE_FETCH |
                               GPU_BARRIER_SHADER_IMAGE_ACCESS);

  /* ================================================================
   * Step 3: Main Rendering Pipeline
   * G-Buffer -> GTAO -> SSGI -> Tiled Deferred Lighting -> Transparent Forward.
   * FIX: was called as pipelines.render(render_view_) — missing combined_fb_ argument. */
  inst_.pipelines.render(render_view_, combined_fb_);

  /* ================================================================
   * Step 4: Post-Processing chain
   *
   * combined_tx  -> Bloom additive -> combined_tx  -> DoF -> postfx_tx_
   *                                                              │
   *                                                        AA or FSR3 below
   *
   * All three textures (combined_tx, postfx_tx_, aa_out_tx_/display_res_tx_)
   * are distinct — necessary to avoid compute shader read/write aliasing. */

  /* Aliasing guards: these are the three points where a future refactor
   * could accidentally pass the same texture as source and destination.
   * The GPU backend does not validate this; the result is undefined. */
  BLI_assert_msg(rbufs.combined_tx.get() != &postfx_tx_,
                 "EEVEE-Game: combined_tx and postfx_tx_ must be distinct");
  BLI_assert_msg(&postfx_tx_ != &aa_out_tx_,
                 "EEVEE-Game: postfx_tx_ and aa_out_tx_ must be distinct");
  BLI_assert_msg(&postfx_tx_ != &display_res_tx_,
                 "EEVEE-Game: postfx_tx_ and display_res_tx_ must be distinct");

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

  /* Return pooled textures to the DRW texture pool for reuse by other engines. */
  inst_.render_buffers.release();
}

} // namespace blender::eevee_game
