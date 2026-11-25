/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_gtao.hh"
#include "eevee_game_instance.hh"

namespace blender::eevee_game {

GTAOModule::GTAOModule(GameInstance &inst) : inst_(&inst) {}

void GTAOModule::init() {
    // Standard AAA Defaults
    settings_.radius = 1.5f;
    settings_.intensity = 1.0f;
    settings_.quality_steps = 6; // Balanced for 60FPS
}

void GTAOModule::sync() {
    int2 display_res = inst_->film.render_extent_get();
    int2 half_res = math::divide_ceil(display_res, int2(2));

    // Allocate Half-Res buffer for the heavy lifting
    // R8 is enough for a grayscale occlusion value
    gtao_lowres_tx_.ensure_2d(gpu::TextureFormat::UNORM_8, half_res, GPU_TEXTURE_USAGE_SHADER_WRITE | GPU_TEXTURE_USAGE_SHADER_READ);

    // Allocate Full-Res buffer for the final result
    gtao_final_tx_.ensure_2d(gpu::TextureFormat::UNORM_8, display_res, GPU_TEXTURE_USAGE_SHADER_WRITE | GPU_TEXTURE_USAGE_SHADER_READ);

}

void GTAOModule::render(View & /*view*/, gpu::Texture *depth_tx) {
    if (!settings_.enabled) return;

    // Pass 1: Main Horizon Search (Half-Res)
    gtao_main_ps_.init();
    gtao_main_ps_.shader_set(inst_->shaders.static_shader_get(SH_GTAO_MAIN));
    gtao_main_ps_.bind_texture("depth_tx", depth_tx);
    /* Shading normals are decoded from the G-Buffer. */
    inst_->render_buffers.gbuffer.bind_resources(gtao_main_ps_);
    gtao_main_ps_.bind_texture("hiz_tx", &inst_->hiz_buffer.front.ref_tx_);
    gtao_main_ps_.bind_image("out_ao_img", gtao_lowres_tx_);
    
    /* Push each field individually: the GLSL shader declares them as separate
     * uniforms (gtao_radius, gtao_falloff, etc.), not as a named struct block. */
    gtao_main_ps_.push_constant("gtao_radius",        settings_.radius);
    gtao_main_ps_.push_constant("gtao_falloff",       settings_.falloff);
    gtao_main_ps_.push_constant("gtao_intensity",     settings_.intensity);
    gtao_main_ps_.push_constant("gtao_quality_steps", settings_.quality_steps);

    /* Camera matrices — required for world-position reconstruction and step sizing. */
    const float4x4 vp_inv = math::invert(
        inst_->uniform_data.projectionmat * inst_->uniform_data.viewmat);
    gtao_main_ps_.push_constant("viewprojinv", vp_inv);
    gtao_main_ps_.push_constant("z_planes",
        float2(inst_->uniform_data.z_near, inst_->uniform_data.z_far));
    gtao_main_ps_.push_constant("screen_res", inst_->uniform_data.screen_res);
    /* The Hi-Z texture is padded; screen UVs must be scaled before sampling it. */
    gtao_main_ps_.push_constant("hiz_uv_scale", inst_->hiz_buffer.uv_scale);
    /* Perspective: a world unit at view distance d covers pixel_scale / d pixels. */
    gtao_main_ps_.push_constant("pixel_scale",
        0.5f * inst_->uniform_data.projectionmat[1][1] * inst_->uniform_data.screen_res.y);
    
    int2 half_res = gtao_lowres_tx_.size().xy();
    gtao_main_ps_.dispatch(math::divide_ceil(half_res, int2(8)));
    
    gtao_main_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
    inst_->manager->submit(gtao_main_ps_);

    // Pass 2: Bilateral Upsample (Half -> Full)
    // Uses depth to ensure AO doesn't "bleed" across geometry edges
    gtao_upsample_ps_.init();
    gtao_upsample_ps_.shader_set(inst_->shaders.static_shader_get(SH_GTAO_UPSAMPLE));
    gtao_upsample_ps_.bind_texture("ao_lowres_tx", gtao_lowres_tx_);
    gtao_upsample_ps_.bind_texture("depth_tx", depth_tx);
    gtao_upsample_ps_.bind_image("out_ao_final_img", gtao_final_tx_);
    
    /* Bilateral upsample also needs z_planes and screen_res to linearise depth
     * for the cross-bilateral weight — declared as uniforms in the GLSL. */
    gtao_upsample_ps_.push_constant("z_planes",
        float2(inst_->uniform_data.z_near, inst_->uniform_data.z_far));
    gtao_upsample_ps_.push_constant("screen_res", inst_->uniform_data.screen_res);

    int2 full_res = gtao_final_tx_.size().xy();
    gtao_upsample_ps_.dispatch(math::divide_ceil(full_res, int2(8)));
    /* The lighting pass samples the result right after. */
    gtao_upsample_ps_.barrier(GPU_BARRIER_SHADER_IMAGE_ACCESS | GPU_BARRIER_TEXTURE_FETCH);
    inst_->manager->submit(gtao_upsample_ps_);
}

} // namespace blender::eevee_game
