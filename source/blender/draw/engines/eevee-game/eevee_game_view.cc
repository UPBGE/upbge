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
 * Must be called every frame before the first submit: it invalidates the
 * view fingerprint, which is what makes the Manager call
 * GameView::compute_visibility() again.
 *
 * View::sync() takes (view matrix, window matrix) and derives the inverses
 * and the frustum planes itself.
 * ================================================================ */

void ShadingView::update_view()
{
  render_view_.sync(inst_.uniform_data.viewmat,
                    inst_.uniform_data.projectionmat);
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
  prepass_fb_.bind();
  /* EEVEE surface shaders write reversed depth (reverse_z::transform in the
   * vertex stage): 1 = near, 0 = far. Clear to far. */
  GPU_framebuffer_clear_depth(prepass_fb_, 0.0f);
  /* Only moving objects use the velocity variant of the depth shader; the
   * others leave the motion buffer untouched, so it starts at zero.
   * TODO: camera motion for static pixels still has to be resolved from
   * depth (EEVEE does it in the film pass) before FSR can rely on it. */
  GPU_framebuffer_clear_color(prepass_fb_, double4(0.0));

  /* Opaque depth + velocity — all materials submit here in sync_mesh().
   * DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_GREATER_EQUAL set per sub-pass
   * in PipelineModule::material_add(). */
  GPU_debug_group_begin("Prepass.Opaque");
  inst_.manager->submit(inst_.pipelines.prepass_opaque_get(), render_view_);
  GPU_debug_group_end();

  /* Hi-Z from opaque depth only, so it has to run before the transparent
   * prepass writes into the same buffer: glass must not occlude what is
   * behind it, neither for next frame's culling nor for SSR / SSGI rays. */
  GPU_debug_group_begin("HiZ.Update");
  inst_.manager->submit(inst_.hiz_update_ps_);
  inst_.hiz_buffer.persmat = render_view_.persmat();
  inst_.hiz_buffer.valid = true;
  GPU_debug_group_end();

  /* Transparent prepass: writes depth so DoF CoC is correct at glass edges.
   * Blended geometry depth is intentionally written here to prevent
   * CoC discontinuities at transparent-opaque boundaries. */
  if (PipelineModule::use_forward_pass) {
    GPU_debug_group_begin("Prepass.Transparent");
    inst_.manager->submit(inst_.pipelines.prepass_transparent_get(), render_view_);
    GPU_debug_group_end();
  }
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

  RenderBuffers &rbufs = inst_.render_buffers;

  /* Usage flags for intermediate post-processing targets:
   *   SHADER_READ  — sampled as a texture by the next pass in the chain.
   *   SHADER_WRITE — written by compute imageStore.
   *   ATTACHMENT   — bound as a framebuffer colour attachment (SMAA raster passes). */
  constexpr eGPUTextureUsage postfx_usage = GPU_TEXTURE_USAGE_SHADER_READ  |
                                            GPU_TEXTURE_USAGE_SHADER_WRITE |
                                            GPU_TEXTURE_USAGE_ATTACHMENT;

  /* postfx_tx_: DoF writes here at render resolution.
   * Input = combined_tx (after bloom composite); distinct to avoid compute aliasing. */
  postfx_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, render_res, postfx_usage);

  /* aa_out_tx_: FXAA/SMAA writes here at render resolution.
   * Separate from postfx_tx_ so SMAA's 3-pass chain has a stable source for each pass. */
  aa_out_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, render_res, postfx_usage);

  /* display_res_tx_: FSR3 upscale output at DISPLAY resolution.
   * Allocated at display_res (not render_res) — this is the larger of the two when
   * FSR is active. When FSR is OFF this equals render_res. */
  display_res_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, display_res, postfx_usage);

  /* Hi-Z pyramid and its single-dispatch down-sample pass. The pass binds
   * the mip views by reference, so it only needs rebuilding here. */
  inst_.hiz_buffer.ensure(render_res);
  {
    HiZBufferPair &hiz = inst_.hiz_buffer;
    PassSimple &pass = inst_.hiz_update_ps_;
    gpu::Shader *sh = inst_.shaders.static_shader_get(SH_HIZ_UPDATE);
    const bool update_mip_0 = true;
    pass.init();
    pass.specialize_constant(sh, "update_mip_0", update_mip_0);
    pass.shader_set(sh);
    pass.bind_ssbo("finished_tile_counter", hiz.tile_counter_);
    pass.bind_texture("depth_tx", &inst_.render_buffers.depth_tx);
    pass.bind_image("out_mip_0", &hiz.mip_ref_[0]);
    pass.bind_image("out_mip_1", &hiz.mip_ref_[1]);
    pass.bind_image("out_mip_2", &hiz.mip_ref_[2]);
    pass.bind_image("out_mip_3", &hiz.mip_ref_[3]);
    pass.bind_image("out_mip_4", &hiz.mip_ref_[4]);
    pass.bind_image("out_mip_5", &hiz.mip_ref_[5]);
    pass.bind_image("out_mip_6", &hiz.mip_ref_[6]);
    pass.dispatch(int3(math::divide_ceil(hiz.tx_.size().xy(), int2(HIZ_GROUP_SIZE)), 1));
    pass.barrier(GPU_BARRIER_TEXTURE_FETCH);
  }

  /* Prepass: depth write + motion vectors (RG16F, read by FSR3). */
  /* Attachment indices are the fragment outputs of `eevee_surf_depth`:
   * 0 = normal, 1 = object id (both unused here), 2 = velocity. */
  prepass_fb_.ensure(GPU_ATTACHMENT_TEXTURE(rbufs.depth_tx),
                     GPU_ATTACHMENT_NONE,
                     GPU_ATTACHMENT_NONE,
                     GPU_ATTACHMENT_TEXTURE(rbufs.vector_tx));

  /* Combined: HDR color for deferred lighting and forward transparency.
   * Depth is attached because the transparent pass depth tests against the
   * opaque scene; the lighting pass is a full-screen draw with no depth test. */
  combined_fb_.ensure(GPU_ATTACHMENT_TEXTURE(rbufs.depth_tx),
                      GPU_ATTACHMENT_TEXTURE(rbufs.combined_tx));
}

gpu::Texture *ShadingView::get_final_texture()
{
  return final_tx_;
}

/* ================================================================
 * render()
 * ================================================================ */

void ShadingView::render()
{
  RenderBuffers &rbufs = inst_.render_buffers;

  /* Sync render_view_ with the current frame's camera matrices. */
  update_view();

  /* ================================================================
   * Steps 1 + 2: Culling, Depth/Velocity Prepass, Hi-Z
   *
   * There is no explicit culling call. The first submit() with
   * render_view_ makes the Manager run GameView::compute_visibility(),
   * and every later pass submitted with the same view reuses that result.
   * The Hi-Z pyramid is rebuilt inside render_prepass(). */
  render_prepass();

  /* ================================================================
   * Step 3: Main Rendering Pipeline
   * G-Buffer -> GTAO -> SSGI -> Tiled Deferred Lighting -> Transparent Forward.
   * FIX: was called as pipelines.render(render_view_) — missing combined_fb_ argument. */
  /* Start the frame's color from EEVEE's "nothing here" value: black with
   * full transmittance. The G-Buffer pass then writes emission with alpha 0
   * for every covered pixel, and the lighting pass adds on top. */
  combined_fb_.bind();
  GPU_framebuffer_clear_color(combined_fb_, double4(0.0, 0.0, 0.0, 1.0));

  inst_.pipelines.render(render_view_, combined_fb_);

  /* ================================================================
   * Step 4: Post-Processing chain
   *
   * combined_tx -> Bloom (in place) -> DoF -> postfx_tx_ -> AA or FSR3
   *
   * Every stage that reads one texture and writes another uses distinct
   * textures; a compute pass must never alias its input and output. */

  /* Bloom: build the downsample/upsample pyramid, then additively composite
   * pyramid[0] back onto combined_tx in a single READ_WRITE compute pass. */
  inst_.bloom.render(rbufs.combined_tx);
  inst_.bloom.composite(rbufs.combined_tx);

  /* DoF: reads combined_tx, writes postfx_tx_. When the camera has no depth
   * of field nothing is written, and the chain keeps reading combined_tx
   * instead of paying a full-screen copy. */
  gpu::Texture *color_tx = rbufs.combined_tx;
  if (inst_.dof.enabled()) {
    inst_.dof.render_fast(color_tx, postfx_tx_);
    color_tx = postfx_tx_;
  }

  /* ================================================================
   * Step 5: Upscaling / Anti-Aliasing */
  /* is_initialized() and not the requested mode: FSR3 is Vulkan only, and on
   * any other backend the context is never created. */
  if (inst_.upscale.is_initialized()) {
    /* FSR3: render resolution in, display resolution out. */
    inst_.upscale.apply_fsr3(color_tx, display_res_tx_, nullptr);
    final_tx_ = display_res_tx_;
  }
  else {
    /* Spatial AA writes aa_out_tx_; with AA off the input is the result. */
    final_tx_ = inst_.pipelines.aa.apply_aa(color_tx, aa_out_tx_, inst_.pipelines.aa_settings);
  }

  /* ================================================================
   * Step 6: Present */
  inst_.film.present(final_tx_);
}

} // namespace blender::eevee_game
