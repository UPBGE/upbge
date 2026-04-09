/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_depth_of_field.hh"
#include "eevee_game_instance.hh"

namespace blender::eevee_game {

DepthOfField::DepthOfField(GameInstance &inst) : inst_(&inst) {}

void DepthOfField::init() {
  // Sync with Blender Camera data
  const Object *cam_ob = inst_->camera_eval_object;
  if (cam_ob && cam_ob->type == OB_CAMERA) {
    const blender::Camera *cam = reinterpret_cast<const blender::Camera *>(cam_ob->data);
    settings_.focus_distance = cam->dof.focus_distance;
    settings_.f_stop = cam->dof.aperture_fstop;
    settings_.enabled = (cam->dof.flag & CAM_DOF_ENABLED);
  }
}

void DepthOfField::sync() {
  if (!settings_.enabled) return;

  int2 render_res = inst_->film.render_extent_get();
  int2 half_res = math::divide_ceil(render_res, int2(2));

  // R16F is enough for CoC values (positive for background blur, negative for foreground)
  coc_tx_ = std::make_unique<gpu::Texture>();
  coc_tx_->ensure_2d(gpu::TextureFormat::SFLOAT_16, render_res, GPU_TEXTURE_USAGE_SHADER_WRITE | GPU_TEXTURE_USAGE_SHADER_READ);

  // Half-res color for the expensive bokeh sampling
  bokeh_half_tx_ = std::make_unique<gpu::Texture>();
  bokeh_half_tx_->ensure_2d(gpu::TextureFormat::RGBA16F, half_res, GPU_TEXTURE_USAGE_SHADER_WRITE | GPU_TEXTURE_USAGE_SHADER_READ);

  // Shader configurations
  coc_setup_ps_.init();
  coc_setup_ps_.shader_set(inst_->shaders.static_shader_get(DOF_FAST_APPROX)); // Sub-mode: Setup
  
  bokeh_blur_ps_.init();
  bokeh_blur_ps_.shader_set(inst_->shaders.static_shader_get(DOF_FAST_APPROX)); // Sub-mode: Blur

  dof_resolve_ps_.init();
  dof_resolve_ps_.shader_set(inst_->shaders.static_shader_get(DOF_FAST_APPROX)); // Sub-mode: Resolve
}

void DepthOfField::render_fast(gpu::Texture *input_color_tx, gpu::Texture *output_color_tx) {
  if (!settings_.enabled) return;

  GPU_debug_group_begin("Depth Of Field (Fast)");

  // 1. COC SETUP
  // Calculate the Circle of Confusion per pixel
  coc_setup_ps_.bind_texture("depth_tx", &inst_->render_buffers.depth_tx);
  coc_setup_ps_.bind_image("out_coc_img", coc_tx_.get());
  coc_setup_ps_.push_constant("focus_params", float4(settings_.focus_distance, settings_.f_stop, settings_.max_bokeh_radius, 0.0f));
  coc_setup_ps_.push_constant("z_planes",
      float2(inst_->uniform_data.z_near, inst_->uniform_data.z_far));

  int2 render_res = coc_tx_->size().xy();
  coc_setup_ps_.dispatch(math::divide_ceil(render_res, int2(8)));
  coc_setup_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);

  // 2. BOKEH BLUR (Half-Res)
  // Standard AAA approach: blur the image in a smaller buffer
  bokeh_blur_ps_.bind_texture("in_color_tx", input_color_tx);
  bokeh_blur_ps_.bind_texture("coc_tx", coc_tx_.get());
  bokeh_blur_ps_.bind_image("out_bokeh_img", bokeh_half_tx_.get());
  
  int2 half_res = bokeh_half_tx_->size().xy();
  bokeh_blur_ps_.dispatch(math::divide_ceil(half_res, int2(8)));
  bokeh_blur_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);

  // 3. RESOLVE
  // Composite half-res blur back to full-res using bilateral filtering to prevent "bleeding"
  dof_resolve_ps_.bind_texture("in_sharp_tx", input_color_tx);
  dof_resolve_ps_.bind_texture("in_bokeh_tx", bokeh_half_tx_.get());
  dof_resolve_ps_.bind_texture("coc_tx", coc_tx_.get());
  dof_resolve_ps_.bind_image("out_final_img", output_color_tx);
  
  dof_resolve_ps_.dispatch(math::divide_ceil(render_res, int2(8)));

  GPU_debug_group_end();
}

} // namespace blender::eevee_game
