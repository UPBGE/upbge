/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_film.hh"
#include "eevee_game_instance.hh"
#include "eevee_game_upscaling.hh"

#include "BKE_compositor.hh"
#include "DNA_camera_types.h"
#include "DNA_world_types.h"
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
      inst_.temporal.enabled() ?
          inst_.temporal.render_res_get(display_extent_) :
          UpscaleModule::calculate_render_res(display_extent_, inst_.upscale_settings.mode),
      int2(1));
}

void Film::sync()
{
  /* No accumulation in game mode, so no film jitter of its own. With FSR the
   * projection jitter comes from UpscaleModule::advance_jitter(). */
  jitter_ = float2(0.0f);

  PassSimple &pass = present_ps_;
  pass.init();
  pass.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_ALWAYS);
  pass.shader_set(inst_.shaders.static_shader_get(SH_FILM_PRESENT));
  pass.bind_texture("color_tx", &present_tx_);
  pass.bind_texture("depth_tx", &inst_.render_buffers.depth_tx);
  pass.push_constant("exposure", &inst_.uniform_data.exposure);
  /* The viewport color buffer is scene linear: Blender applies the view
   * transform when it composites the viewport, so doing it here as well
   * would tone map twice. A standalone swapchain target would enable both. */
  pass.push_constant("do_tonemapping", 0);
  pass.push_constant("do_gamma_encode", 0);
  pass.draw_procedural(GPU_PRIM_TRIS, 1, 3);
}

void Film::write_viewport_compositor_passes()
{
  const DRWContext *draw_ctx = inst_.draw_ctx;
  use_mist_pass_ = draw_ctx != nullptr && draw_ctx->viewport != nullptr &&
                   draw_ctx->is_viewport_compositor_enabled() &&
                   bke::compositor::get_used_passes(*inst_.scene, inst_.view_layer)
                       .contains(RE_PASSNAME_MIST);
  if (!use_mist_pass_) {
    return;
  }

  /* EEVEE's mist mapping (Film::sync_mist()): the world settings, else the clip range. */
  const World *world = inst_.scene->world;
  const UniformData &ud = inst_.uniform_data;
  const float clip_near = ud.z_near;
  const float clip_far = ud.z_far;
  const float mist_start = world ? world->miststa : clip_near;
  const float mist_distance = max_ff(world ? world->mistdist : fabsf(clip_far - clip_near),
                                     1e-8f);
  float exponent = 1.0f;
  switch (world ? world->mistype : int(WO_MIST_LINEAR)) {
    case WO_MIST_QUADRATIC:
      exponent = 2.0f;
      break;
    case WO_MIST_INVERSE_QUADRATIC:
      exponent = 0.5f;
      break;
    default:
      break;
  }
  const bool is_persp = ud.projectionmat[3][3] == 0.0f;

  /* The compositor expects its passes at the viewport size. */
  draw::TextureFromPool &mist_tx = DRW_viewport_pass_texture_get(RE_PASSNAME_MIST);
  mist_tx.acquire(display_extent_, gpu::TextureFormat::SFLOAT_16);

  PassSimple &pass = mist_ps_;
  pass.init();
  pass.shader_set(inst_.shaders.static_shader_get(SH_FILM_MIST));
  pass.bind_texture("depth_tx", &inst_.render_buffers.depth_tx);
  pass.bind_image("out_mist_img", mist_tx);
  pass.push_constant("mist_params",
                     float4(1.0f / mist_distance,
                            -mist_start / mist_distance,
                            exponent,
                            is_persp ? 1.0f : 0.0f));
  pass.push_constant("ray_scale",
                     float2(1.0f / ud.projectionmat[0][0], 1.0f / ud.projectionmat[1][1]));
  pass.push_constant("z_planes", float2(clip_near, clip_far));
  pass.dispatch(math::divide_ceil(display_extent_, int2(8)));
  pass.barrier(GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_SHADER_IMAGE_ACCESS);
  inst_.manager->submit(pass);
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
