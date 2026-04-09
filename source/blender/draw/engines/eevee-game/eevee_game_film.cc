/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_film.hh"
#include "eevee_game_instance.hh"
#include "eevee_game_upscaling.hh"

#include "BLI_halton.h"   /* BLI_halton_1d() */
#include "DRW_render.hh"

namespace blender::eevee_game {

void Film::init(const int2 &display_extent)
{
  display_extent_ = display_extent;

  /* Render resolution is either full display resolution (FSR OFF)
   * or a downscaled resolution computed by the SDK (FSR ON). */
  render_extent_ = UpscaleModule::calculate_render_res(
      display_extent, inst_.upscale_settings.mode);

  if (inst_.view_layer != nullptr) {
    enabled_passes_ = eViewLayerEEVEEPassType(inst_.view_layer->eevee.render_passes);
  }
}

void Film::sync()
{
  if (inst_.upscale_settings.mode != UpscaleMode::OFF) {
    /* When FSR is active the jitter sequence is driven by the SDK itself
     * inside UpscaleModule::apply_fsr3() via ffxFsr3GetJitterOffset().
     * apply_fsr3() writes the computed values directly into inst_.uniform_data.jitter
     * so they are available to update_view() for the projection matrix offset.
     *
     * We set jitter_ to zero here so the prepass and G-Buffer passes — which
     * run BEFORE apply_fsr3() updates uniform_data — don't inherit a stale value
     * from the previous frame. apply_fsr3() will overwrite it before present(). */
    jitter_ = float2(0.0f);
  }
  else {
    /* FSR OFF: no temporal reconstruction, no jitter needed.
     * SMAA/FXAA are purely spatial and don't use sub-pixel offsets. */
    jitter_ = float2(0.0f);
  }
}

void Film::present(gpu::Texture *final_color_tx)
{
  /* Game mode: no accumulation. Single blit to the viewport backbuffer.
   * GPU_texture_copy(dst, src) — destination is the first argument. */
  DefaultFramebufferList *dfbl = DRW_viewport_framebuffer_list_get();
  GPU_framebuffer_bind(dfbl->default_fb);

  DefaultTextureList *dtxl = DRW_viewport_texture_list_get();
  GPU_texture_copy(dtxl->color, final_color_tx);
}

} // namespace blender::eevee_game
