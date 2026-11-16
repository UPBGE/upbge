/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_raytrace.hh"
#include "eevee_game_instance.hh"

namespace blender::eevee_game {

RayTraceModule::RayTraceModule(GameInstance *inst) : inst_(inst) {}

RayTraceModule::~RayTraceModule() {}

void RayTraceModule::init() {
    // Initialize settings from the game's config
    settings_.max_roughness = 0.5f;
    settings_.max_steps = 64;
    settings_.thickness = 0.1f;
    settings_.edge_fade = 0.1f;
}

void RayTraceModule::sync()
{
  /* Nothing to build yet. SH_SSR_TRACE used to alias EEVEE's
   * `eevee_ray_trace_screen`, whose interface (ray data buffers, tile lists,
   * closure bins, sampling UBO) has nothing in common with the two textures
   * bound here, so the pass could never have linked. The module stays wired
   * into the instance so a dedicated trace shader can be dropped in. */
}

void RayTraceModule::render(gpu::Texture *radiance_tx, gpu::Texture *out_radiance)
{
  if (!settings_.enabled) {
    return;
  }

  ssr_ps_.init();
  ssr_ps_.shader_set(inst_->shaders.static_shader_get(SH_SSR_TRACE));
  ssr_ps_.bind_texture("hiz_tx", &inst_->hiz_buffer.front.ref_tx_);
  ssr_ps_.bind_texture("in_radiance_tx", radiance_tx);
  ssr_ps_.bind_image("out_radiance_img", out_radiance);
  ssr_ps_.push_constant("hiz_uv_scale", inst_->hiz_buffer.uv_scale);
  ssr_ps_.dispatch(
      math::divide_ceil(inst_->film.render_extent_get(), int2(RAYTRACE_GROUP_SIZE)));
  /* Reflections must be visible to the combine pass that samples them. */
  ssr_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);

  inst_->manager->submit(ssr_ps_);
}

} // namespace blender::eevee_game