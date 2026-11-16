/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_depth_of_field.hh"
#include "eevee_game_instance.hh"

namespace blender::eevee_game {

DepthOfField::DepthOfField(GameInstance &inst) : inst_(&inst) {}

void DepthOfField::init() {}

void DepthOfField::sync()
{
  /* Camera settings can be animated or changed by game logic, so they are
   * read every frame and not once at startup. */
  settings_.enabled = false;
  const Object *cam_ob = inst_->camera_eval_object;
  if (cam_ob && cam_ob->type == OB_CAMERA) {
    const blender::Camera &cam = DRW_object_get_data_for_drawing<const blender::Camera>(*cam_ob);
    settings_.focus_distance = cam.dof.focus_distance;
    settings_.f_stop = cam.dof.aperture_fstop;
    settings_.focal_length = cam.lens;
    settings_.sensor_size = cam.sensor_x;
    settings_.enabled = (cam.dof.flag & CAM_DOF_ENABLED) != 0;
  }
  if (!settings_.enabled) {
    return;
  }

  const int2 render_res = inst_->film.render_extent_get();
  const int2 half_res = math::divide_ceil(render_res, int2(2));
  const eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_WRITE | GPU_TEXTURE_USAGE_SHADER_READ;

  /* R16F is enough for CoC (positive = background blur, negative = foreground). */
  coc_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_16, render_res, usage);
  /* Half-res color for the expensive bokeh gather. */
  bokeh_half_tx_.ensure_2d(gpu::TextureFormat::SFLOAT_16_16_16_16, half_res, usage);
}

void DepthOfField::render_fast(gpu::Texture *input_color_tx, gpu::Texture *output_color_tx) {
  if (!settings_.enabled) return;

  const float4 focus_params = float4(
      settings_.focus_distance, settings_.f_stop, settings_.max_bokeh_radius, 0.0f);
  const float2 z_planes = float2(inst_->uniform_data.z_near, inst_->uniform_data.z_far);

  // 1. COC SETUP
  // Calculate the Circle of Confusion per pixel
  coc_setup_ps_.init();
  coc_setup_ps_.shader_set(inst_->shaders.static_shader_get(SH_DOF_COC_SETUP));
  coc_setup_ps_.bind_texture("depth_tx", &inst_->render_buffers.depth_tx);
  coc_setup_ps_.bind_image("out_coc_img", coc_tx_);
  coc_setup_ps_.push_constant("focus_params", focus_params);
  coc_setup_ps_.push_constant("z_planes", z_planes);

  int2 render_res = coc_tx_.size().xy();
  coc_setup_ps_.dispatch(math::divide_ceil(render_res, int2(8)));
  coc_setup_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  inst_->manager->submit(coc_setup_ps_);

  // 2. BOKEH BLUR (Half-Res)
  // Standard AAA approach: blur the image in a smaller buffer
  bokeh_blur_ps_.init();
  bokeh_blur_ps_.shader_set(inst_->shaders.static_shader_get(SH_DOF_BOKEH_BLUR));
  bokeh_blur_ps_.bind_texture("in_color_tx", input_color_tx);
  bokeh_blur_ps_.bind_texture("coc_tx", coc_tx_);
  bokeh_blur_ps_.bind_image("out_bokeh_img", bokeh_half_tx_);
  bokeh_blur_ps_.push_constant("focus_params", focus_params);
  bokeh_blur_ps_.push_constant("z_planes", z_planes);
  
  int2 half_res = bokeh_half_tx_.size().xy();
  bokeh_blur_ps_.dispatch(math::divide_ceil(half_res, int2(8)));
  bokeh_blur_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  inst_->manager->submit(bokeh_blur_ps_);

  // 3. RESOLVE
  // Composite half-res blur back to full-res using bilateral filtering to prevent "bleeding"
  dof_resolve_ps_.init();
  dof_resolve_ps_.shader_set(inst_->shaders.static_shader_get(SH_DOF_RESOLVE));
  dof_resolve_ps_.bind_texture("in_sharp_tx", input_color_tx);
  dof_resolve_ps_.bind_texture("in_bokeh_tx", bokeh_half_tx_);
  dof_resolve_ps_.bind_texture("coc_tx", coc_tx_);
  dof_resolve_ps_.bind_image("out_final_img", output_color_tx);
  
  dof_resolve_ps_.push_constant("focus_params", focus_params);
  dof_resolve_ps_.push_constant("z_planes", z_planes);
  dof_resolve_ps_.dispatch(math::divide_ceil(render_res, int2(8)));
  /* The AA / upscale pass samples the result right after. */
  dof_resolve_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
  inst_->manager->submit(dof_resolve_ps_);
}

} // namespace blender::eevee_game
