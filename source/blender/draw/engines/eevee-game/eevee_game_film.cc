/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_film.hh"
#include "eevee_game_instance.hh"
#include "eevee_game_upscaling.hh"

#include "DRW_render.hh"
#include "draw_view_data.hh" /* DefaultFramebufferList */

namespace blender::eevee_game {

void Film::init(const int2 &display_extent)
{
  /* A minimised window reports a zero extent. */
  display_extent_ = math::max(display_extent, int2(1));

  /* Render resolution is either full display resolution (FSR OFF)
   * or the reduced resolution of the FSR mode. */
  render_extent_ = math::max(
      UpscaleModule::calculate_render_res(display_extent_, inst_.upscale_settings.mode), int2(1));
}

void Film::sync()
{
  /* No accumulation in game mode, so no film jitter of its own. With FSR the
   * projection jitter comes from UpscaleModule::advance_jitter(). */
  jitter_ = float2(0.0f);

  PassSimple &pass = present_ps_;
  pass.init();
  pass.state_set(DRW_STATE_WRITE_COLOR);
  pass.shader_set(inst_.shaders.static_shader_get(SH_FILM_PRESENT));
  pass.bind_texture("color_tx", &present_tx_);
  pass.push_constant("exposure", &inst_.uniform_data.exposure);
  /* The viewport color buffer is scene linear: Blender applies the view
   * transform when it composites the viewport, so doing it here as well
   * would tone map twice. A standalone swapchain target would enable both. */
  pass.push_constant("do_tonemapping", 0);
  pass.push_constant("do_gamma_encode", 0);
  pass.draw_procedural(GPU_PRIM_TRIS, 1, 3);
}

void Film::present(gpu::Texture *final_color_tx)
{
  /* Offline (F12) renders have no viewport; the result is read back from
   * the final texture in GameInstance::render_frame(). */
  if (inst_.draw_ctx == nullptr || inst_.draw_ctx->viewport == nullptr) {
    return;
  }

  present_tx_ = final_color_tx;

  DefaultFramebufferList *dfbl = inst_.draw_ctx->viewport_framebuffer_list_get();
  GPU_framebuffer_bind(dfbl->default_fb);
  inst_.manager->submit(present_ps_);
}

} // namespace blender::eevee_game
