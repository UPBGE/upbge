/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_ssgi.hh"
#include "eevee_game_instance.hh"

namespace blender::eevee_game {

SSGIModule::SSGIModule(GameInstance &inst) : inst_(&inst) {}

void SSGIModule::init() {
    settings_.intensity = 1.0f;
    settings_.radius = 2.0f;
    settings_.quality_steps = 8; 
}

void SSGIModule::sync() {
    int2 display_res = inst_->film.render_extent_get();
    int2 half_res = math::divide_ceil(display_res, int2(2));

    // Allocate Half-Res buffer for the bounce color (RGB16F)
    ssgi_lowres_tx_ = std::make_unique<gpu::Texture>();
    ssgi_lowres_tx_->ensure_2d(gpu::TextureFormat::RGBA16F, half_res, GPU_TEXTURE_USAGE_SHADER_WRITE | GPU_TEXTURE_USAGE_SHADER_READ);

    // Allocate Full-Res buffer for the final result
    ssgi_final_tx_ = std::make_unique<gpu::Texture>();
    ssgi_final_tx_->ensure_2d(gpu::TextureFormat::RGBA16F, display_res, GPU_TEXTURE_USAGE_SHADER_WRITE | GPU_TEXTURE_USAGE_SHADER_READ);

    ssgi_main_ps_.init();
    ssgi_main_ps_.shader_set(inst_->shaders.static_shader_get(SH_SSGI_MAIN));

    ssgi_blur_ps_.init();
    ssgi_blur_ps_.shader_set(inst_->shaders.static_shader_get(SH_SSGI_BLUR));
}

void SSGIModule::render(View &view, gpu::Texture *scene_color_tx, gpu::Texture *depth_tx, gpu::Texture *normal_tx) {
    if (!settings_.enabled) return;

    GPU_debug_group_begin("SSGI");

    // Pass 1: Main Trace (Using Hi-Z)
    // We sample the scene_color_tx (radiance) and "bounce" it based on surface normals
    ssgi_main_ps_.bind_texture("scene_color_tx", scene_color_tx);
    ssgi_main_ps_.bind_texture("depth_tx", depth_tx);
    ssgi_main_ps_.bind_texture("normal_tx", normal_tx);
    ssgi_main_ps_.bind_texture("hiz_tx", &inst_->hiz_buffer.front.ref_tx_);
    ssgi_main_ps_.bind_image("out_ssgi_img", ssgi_lowres_tx_.get());
    
    /* Each uniform is declared separately in the GLSL shader */
    ssgi_main_ps_.push_constant("ssgi_intensity",         settings_.intensity);
    ssgi_main_ps_.push_constant("ssgi_radius",            settings_.radius);
    ssgi_main_ps_.push_constant("ssgi_color_saturation",  settings_.color_saturation);
    ssgi_main_ps_.push_constant("ssgi_quality_steps",     settings_.quality_steps);

    /* Camera data for world-pos reconstruction and reprojection. */
    const float4x4 vp_inv = math::invert(
        inst_->uniform_data.projectionmat * inst_->uniform_data.viewmat);
    ssgi_main_ps_.push_constant("viewprojinv",  vp_inv);
    ssgi_main_ps_.push_constant("viewproj",
        inst_->uniform_data.projectionmat * inst_->uniform_data.viewmat);
    ssgi_main_ps_.push_constant("z_planes",
        float2(inst_->uniform_data.z_near, inst_->uniform_data.z_far));
    ssgi_main_ps_.push_constant("screen_res",   inst_->uniform_data.screen_res);
    ssgi_main_ps_.push_constant("frame_count",  inst_->uniform_data.frame_count);
    
    int2 half_res = ssgi_lowres_tx_->size().xy();
    ssgi_main_ps_.dispatch(math::divide_ceil(half_res, int2(8)));
    
    ssgi_main_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);

    // Pass 2: Bilateral Blur & Upsample
    // Merges the noisy low-res bounce into the full-res scene
    ssgi_blur_ps_.bind_texture("ssgi_lowres_tx", ssgi_lowres_tx_.get());
    ssgi_blur_ps_.bind_texture("depth_tx", depth_tx);
    ssgi_blur_ps_.bind_image("out_ssgi_final_img", ssgi_final_tx_.get());
    
    ssgi_blur_ps_.push_constant("z_planes",
        float2(inst_->uniform_data.z_near, inst_->uniform_data.z_far));
    ssgi_blur_ps_.push_constant("screen_res", inst_->uniform_data.screen_res);

    int2 full_res = ssgi_final_tx_->size().xy();
    ssgi_blur_ps_.dispatch(math::divide_ceil(full_res, int2(8)));

    GPU_debug_group_end();
}

} // namespace blender::eevee_game
