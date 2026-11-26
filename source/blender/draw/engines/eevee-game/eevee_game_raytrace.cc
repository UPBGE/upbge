/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_raytrace.hh"
#include "eevee_game_instance.hh"

namespace blender::eevee_game {

RayTraceModule::RayTraceModule(GameInstance *inst) : inst_(inst) {}

void RayTraceModule::init() {}

void RayTraceModule::sync()
{
  const int2 extent = inst_->film.render_extent_get();
  const eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_WRITE | GPU_TEXTURE_USAGE_SHADER_READ;
  const int2 half_res = math::divide_ceil(extent, int2(2));
  ssr_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, half_res, usage);
  ssr_filtered_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, half_res, usage);
}

void RayTraceModule::render(gpu::Texture *scene_color_tx, gpu::Texture *depth_tx)
{
  if (!settings_.enabled) {
    return;
  }
  const UniformData &ud = inst_->uniform_data;
  const float4x4 viewproj = ud.projectionmat * ud.viewmat;
  if (!inst_->frame_state.history_valid) {
    /* No radiance to reflect yet: the lighting pass keeps the world reflection. */
    viewproj_prev_ = viewproj;
    ssr_filtered_tx_.clear(float4(0.0f));
    return;
  }

  {
    PassSimple &pass = ssr_trace_ps_;
    pass.init();
    pass.shader_set(inst_->shaders.static_shader_get(SH_SSR_TRACE));
    pass.bind_texture("scene_color_tx", scene_color_tx);
    pass.bind_texture("depth_tx", depth_tx);
    pass.bind_texture("hiz_tx", &inst_->hiz_buffer.front.ref_tx_);
    inst_->render_buffers.gbuffer.bind_resources(pass);
    pass.bind_image("out_ssr_img", ssr_tx_);
    pass.push_constant("ssr_max_roughness", settings_.max_roughness);
    pass.push_constant("ssr_thickness", settings_.thickness);
    pass.push_constant("ssr_max_steps", settings_.max_steps);
    pass.push_constant("viewprojinv", math::invert(viewproj));
    pass.push_constant("viewproj", viewproj);
    /* scene_color_tx is last frame's: hits are looked up where they were then. */
    pass.push_constant("viewproj_prev", viewproj_prev_);
    pass.push_constant("camera_pos", ud.camera_pos);
    pass.push_constant("z_planes", float2(ud.z_near, ud.z_far));
    pass.push_constant("screen_res", ud.screen_res);
    pass.push_constant("hiz_uv_scale", inst_->hiz_buffer.uv_scale);
    pass.dispatch(math::divide_ceil(ssr_tx_.size().xy(), int2(8)));
    pass.barrier(GPU_BARRIER_TEXTURE_FETCH);
    inst_->manager->submit(pass);
  }
  viewproj_prev_ = viewproj;

  {
    /* The SSGI filter: both are (radiance, weight) at half resolution. A single mirror ray
     * per half resolution pixel aliases on curved surfaces without it. */
    PassSimple &pass = ssr_filter_ps_;
    pass.init();
    pass.shader_set(inst_->shaders.static_shader_get(SH_SSGI_BLUR));
    pass.bind_texture("ssgi_lowres_tx", ssr_tx_);
    pass.bind_texture("hiz_tx", &inst_->hiz_buffer.front.ref_tx_);
    pass.bind_image("out_ssgi_img", ssr_filtered_tx_);
    pass.push_constant("z_planes", float2(ud.z_near, ud.z_far));
    pass.dispatch(math::divide_ceil(ssr_filtered_tx_.size().xy(), int2(8)));
    /* The lighting pass samples the result right after. */
    pass.barrier(GPU_BARRIER_TEXTURE_FETCH);
    inst_->manager->submit(pass);
  }
}

}  // namespace blender::eevee_game
