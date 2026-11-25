/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * FSR 3.1 host: port of FidelityFX's ffx_fsr3upscaler.cpp (SDK v1.1.4, MIT), fsr3upscalerCreate()
 * and fsr3upscalerDispatch(). Same resources, constants and pass order; the passes are recorded
 * once per resolution and only their inputs change from frame to frame.
 */

#include <cfloat>
#include <cmath>

#include "BLI_math_base.hh"
#include "BLI_math_bits.h"
#include "BLI_math_half.hh"
#include "BLI_vector.hh"

#include "GPU_texture.hh"

#include "DNA_scene_types.h"

#include "eevee_game_instance.hh"
#include "eevee_game_upscaling.hh"

namespace blender::eevee_game {

static float halton(int index, int base)
{
  float f = 1.0f, result = 0.0f;
  for (int i = index; i > 0; i /= base) {
    f /= float(base);
    result += f * float(i % base);
  }
  return result;
}

/* ffxFsr3UpscalerGetJitterPhaseCount(): 8 samples per output pixel area. */
static int jitter_phase_count_get(int render_width, int display_width)
{
  return int(8.0f * std::pow(float(display_width) / float(render_width), 2.0f));
}

/* ffxFsr3UpscalerGetUpscaleRatioFromQualityMode(). */
static float upscale_ratio_get(UpscaleMode mode)
{
  switch (mode) {
    case UpscaleMode::FSR3_ULTRA_QUALITY:
      return 1.3f;
    case UpscaleMode::FSR3_QUALITY:
      return 1.5f;
    case UpscaleMode::FSR3_BALANCED:
      return 1.7f;
    case UpscaleMode::FSR3_PERFORMANCE:
      return 2.0f;
    case UpscaleMode::OFF:
      break;
  }
  return 1.0f;
}

/* static */
int2 UpscaleModule::calculate_render_res(int2 display_res, UpscaleMode mode)
{
  const float ratio = upscale_ratio_get(mode);
  return int2(int(float(display_res.x) / ratio), int(float(display_res.y) / ratio));
}

void UpscaleModule::init(int2 render_res, int2 display_res)
{
  render_res_ = render_res;
  display_res_ = display_res;
  const int2 half_res = render_res / 2;

  constexpr eGPUTextureUsage rw = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_SHADER_WRITE;
  /* GPU_texture_clear() goes through a frame-buffer on OpenGL. */
  constexpr eGPUTextureUsage rw_clear = rw | GPU_TEXTURE_USAGE_ATTACHMENT;
  using TF = gpu::TextureFormat;

  for (int i = 0; i < 2; i++) {
    accumulation_tx_[i].ensure_2d(TF::UNORM_8, render_res, rw_clear);
    luma_tx_[i].ensure_2d(TF::SFLOAT_16, render_res, rw_clear);
    luma_history_tx_[i].ensure_2d(TF::SFLOAT_16_16_16_16, render_res, rw_clear);
    upscaled_tx_[i].ensure_2d(TF::SFLOAT_16_16_16_16, display_res, rw_clear);
  }
  new_locks_tx_.ensure_2d(TF::UNORM_8, display_res, rw_clear);
  intermediate_tx_.ensure_2d(TF::SFLOAT_16, render_res, rw);
  shading_change_tx_.ensure_2d(TF::UNORM_8, half_res, rw);
  /* The pyramids write mips 0 to 5 (FidelityFX allocates the whole chain, never bound past 5),
   * the shading change samples the first ones. */
  const int spd_mip_len = math::min(6, int(std::log2(float(math::max(half_res.x, half_res.y)))) + 1);
  spd_mips_tx_.ensure_2d(
      TF::SFLOAT_16_16, half_res, rw, nullptr, spd_mip_len);
  spd_mips_tx_.ensure_mip_views();
  /* FidelityFX clears the pyramid every frame so that texels it never writes read 0. The
   * written set only depends on the render size, fixed until the next init(): clear once. */
  {
    Vector<float2> zeros(half_res.x * half_res.y, float2(0.0f));
    for (int i = 0; i < 6; i++) {
      /* Missing levels (tiny resolutions) alias the last one: never read past it. */
      const int mip = math::min(i, spd_mip_len - 1);
      spd_mip_[i] = spd_mips_tx_.mip_view(mip);
      if (i == mip) {
        GPU_texture_update_mipmap(spd_mips_tx_, mip, GPU_DATA_FLOAT, zeros.data());
      }
    }
  }
  farthest_depth_mip1_tx_.ensure_2d(TF::SFLOAT_16, half_res, rw);
  dilated_reactive_tx_.ensure_2d(TF::UNORM_8_8_8_8, render_res, rw);
  dilated_motion_tx_.ensure_2d(TF::SFLOAT_16_16, render_res, rw);
  dilated_depth_tx_.ensure_2d(TF::SFLOAT_32, render_res, rw);
  reconstructed_depth_tx_.ensure_2d(
      TF::UINT_32, render_res, rw_clear | GPU_TEXTURE_USAGE_ATOMIC);
  const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  empty_mask_tx_.ensure_2d(TF::UNORM_8, int2(1), GPU_TEXTURE_USAGE_SHADER_READ, zero);

  /* FidelityFX leaves the histories uninitialized: the first frame resets them, but freshly
   * allocated memory can hold NaN that a bilinear tap at the border would pick up. */
  for (int i = 0; i < 2; i++) {
    luma_history_tx_[i].clear(float4(0.0f));
    upscaled_tx_[i].clear(float4(0.0f));
  }
  new_locks_tx_.clear(float4(0.0f));
  spd_atomic_buf_.clear_to_zero();

  /* ffxSpdSetup() over the render resolution. */
  spd_group_count_ = int2((render_res.x - 1) / 64 + 1, (render_res.y - 1) / 64 + 1);

  jitter_phase_count_ = jitter_phase_count_get(render_res.x, display_res.x);
  jitter_index_ = 0;
  frame_ = 0;
  first_execution_ = true;
  data_.previous_frame_jitter = float2(0.0f);
  data_.jitter = float2(0.0f);
  data_.render_size = int2(0);
  data_.upscale_size = int2(0);

  passes_sync();
  is_initialized_ = true;
}

float2 UpscaleModule::advance_jitter()
{
  if (!is_initialized_) {
    return float2(0.0f);
  }
  /* ffxFsr3UpscalerGetJitterOffset(). */
  const int index = (jitter_index_ % jitter_phase_count_) + 1;
  jitter_index_ = (jitter_index_ + 1) % jitter_phase_count_;
  jitter_px_ = float2(halton(index, 2), halton(index, 3)) - 0.5f;
  return 2.0f * jitter_px_ / float2(render_res_);
}

void UpscaleModule::passes_sync()
{
  ShaderModule &shaders = inst_->shaders;
  /* FidelityFX samples with linear filtering and clamp. texelFetch ignores the sampler, except
   * that integer textures with linear filtering are incomplete on OpenGL. */
  const GPUSamplerState linear = {GPU_SAMPLER_FILTERING_LINEAR};
  const GPUSamplerState linear_mip = {GPU_SAMPLER_FILTERING_LINEAR | GPU_SAMPLER_FILTERING_MIPMAP};
  const GPUSamplerState nearest = GPUSamplerState::default_sampler();
  /* Every pass reads what the previous ones wrote, as textures, images or buffers. */
  const GPUBarrier barrier = GPU_BARRIER_TEXTURE_FETCH | GPU_BARRIER_SHADER_IMAGE_ACCESS |
                             GPU_BARRIER_SHADER_STORAGE;
  const int3 render_groups = int3(math::divide_ceil(render_res_, int2(FSR3_GROUP_SIZE)), 1);
  const int3 display_groups = int3(math::divide_ceil(display_res_, int2(FSR3_GROUP_SIZE)), 1);
  const int3 half_groups = int3(math::divide_ceil(render_res_ / 2, int2(FSR3_GROUP_SIZE)), 1);
  const int3 spd_groups = int3(spd_group_count_, 1);
  const int3 rcas_groups = int3(math::divide_ceil(display_res_, int2(FSR3_RCAS_TILE_SIZE)), 1);

  {
    PassSimple &ps = prepare_ps_;
    ps.init();
    /* By slot (eevee_game_fsr3_common): a name needs the shader, which the sub-passes set. */
    ps.bind_ubo(0, data_);
    {
      PassSimple::Sub &sub = ps.sub("PrepareInputs");
      sub.shader_set(shaders.static_shader_get(SH_FSR3_PREPARE_INPUTS));
      sub.bind_texture("r_input_motion_vectors", &motion_tx_, linear);
      sub.bind_texture("r_input_depth", &depth_tx_, linear);
      sub.bind_texture("r_input_color_jittered", &color_tx_, linear);
      sub.bind_image("rw_dilated_motion_vectors", &dilated_motion_tx_);
      sub.bind_image("rw_dilated_depth", &dilated_depth_tx_);
      sub.bind_image("rw_reconstructed_previous_nearest_depth", &reconstructed_depth_tx_);
      sub.bind_image("rw_farthest_depth", &intermediate_tx_);
      sub.bind_image("rw_current_luma", &luma_current_);
      sub.dispatch(render_groups);
      sub.barrier(barrier);
    }
    {
      PassSimple::Sub &sub = ps.sub("LumaPyramid");
      sub.shader_set(shaders.static_shader_get(SH_FSR3_LUMA_PYRAMID));
      sub.bind_texture("r_current_luma", &luma_current_, linear);
      sub.bind_texture("r_farthest_depth", &intermediate_tx_, linear);
      sub.bind_ssbo("rw_spd_global_atomic", spd_atomic_buf_);
      sub.bind_ssbo("rw_frame_info", frame_info_buf_);
      /* Image slots 0 to 5: rw_spd_mip0 to rw_spd_mip5. */
      for (int i = 0; i < 6; i++) {
        sub.bind_image(i, &spd_mip_[i]);
      }
      sub.bind_image("rw_farthest_depth_mip1", &farthest_depth_mip1_tx_);
      sub.dispatch(spd_groups);
      sub.barrier(barrier);
    }
    {
      PassSimple::Sub &sub = ps.sub("ShadingChangePyramid");
      sub.shader_set(shaders.static_shader_get(SH_FSR3_SHADING_CHANGE_PYRAMID));
      sub.bind_texture("r_current_luma", &luma_current_, linear);
      sub.bind_texture("r_previous_luma", &luma_previous_, linear);
      sub.bind_texture("r_dilated_motion_vectors", &dilated_motion_tx_, linear);
      sub.bind_ssbo("rw_spd_global_atomic", spd_atomic_buf_);
      /* Image slots 0 to 5: rw_spd_mip0 to rw_spd_mip5. */
      for (int i = 0; i < 6; i++) {
        sub.bind_image(i, &spd_mip_[i]);
      }
      sub.dispatch(spd_groups);
      sub.barrier(barrier);
    }
    {
      PassSimple::Sub &sub = ps.sub("ShadingChange");
      sub.shader_set(shaders.static_shader_get(SH_FSR3_SHADING_CHANGE));
      sub.bind_texture("r_spd_mips", &spd_mips_tx_, linear_mip);
      sub.bind_image("rw_shading_change", &shading_change_tx_);
      sub.dispatch(half_groups);
      sub.barrier(barrier);
    }
    {
      PassSimple::Sub &sub = ps.sub("PrepareReactivity");
      sub.shader_set(shaders.static_shader_get(SH_FSR3_PREPARE_REACTIVITY));
      sub.bind_texture(
          "r_reconstructed_previous_nearest_depth", &reconstructed_depth_tx_, nearest);
      sub.bind_texture("r_dilated_motion_vectors", &dilated_motion_tx_, linear);
      sub.bind_texture("r_dilated_depth", &dilated_depth_tx_, linear);
      sub.bind_texture("r_reactive_mask", &reactive_tx_, linear);
      sub.bind_texture("r_transparency_and_composition_mask", &empty_mask_tx_, linear);
      sub.bind_texture("r_accumulation", &accumulation_srv_, linear);
      sub.bind_texture("r_shading_change", &shading_change_tx_, linear);
      sub.bind_texture("r_current_luma", &luma_current_, linear);
      sub.bind_image("rw_dilated_reactive_masks", &dilated_reactive_tx_);
      sub.bind_image("rw_new_locks", &new_locks_tx_);
      sub.bind_image("rw_accumulation", &accumulation_uav_);
      sub.dispatch(render_groups);
      sub.barrier(barrier);
    }
    {
      PassSimple::Sub &sub = ps.sub("LumaInstability");
      sub.shader_set(shaders.static_shader_get(SH_FSR3_LUMA_INSTABILITY));
      sub.bind_texture("r_dilated_reactive_masks", &dilated_reactive_tx_, linear);
      sub.bind_texture("r_dilated_motion_vectors", &dilated_motion_tx_, linear);
      sub.bind_texture("r_luma_history", &luma_history_srv_, linear);
      sub.bind_texture("r_farthest_depth_mip1", &farthest_depth_mip1_tx_, linear);
      sub.bind_texture("r_current_luma", &luma_current_, linear);
      sub.bind_ssbo("r_frame_info", frame_info_buf_);
      sub.bind_image("rw_luma_history", &luma_history_uav_);
      sub.bind_image("rw_luma_instability", &intermediate_tx_);
      sub.dispatch(render_groups);
      sub.barrier(barrier);
    }
  }

  for (const bool sharpen : {false, true}) {
    PassSimple &ps = sharpen ? accumulate_sharpen_ps_ : accumulate_ps_;
    ps.init();
    /* By slot (eevee_game_fsr3_common): a name needs the shader, which the sub-passes set. */
    ps.bind_ubo(0, data_);
    {
      PassSimple::Sub &sub = ps.sub("Accumulate");
      sub.shader_set(shaders.static_shader_get(sharpen ? SH_FSR3_ACCUMULATE_SHARPEN :
                                                         SH_FSR3_ACCUMULATE));
      sub.bind_texture("r_dilated_reactive_masks", &dilated_reactive_tx_, linear);
      sub.bind_texture("r_dilated_motion_vectors", &dilated_motion_tx_, linear);
      sub.bind_texture("r_internal_upscaled_color", &upscaled_srv_, linear);
      sub.bind_texture("r_farthest_depth_mip1", &farthest_depth_mip1_tx_, linear);
      sub.bind_texture("r_current_luma", &luma_current_, linear);
      sub.bind_texture("r_luma_instability", &intermediate_tx_, linear);
      sub.bind_texture("r_input_color_jittered", &color_tx_, linear);
      sub.bind_image("rw_internal_upscaled_color", &upscaled_uav_);
      sub.bind_image("rw_upscaled_output", &output_tx_);
      sub.bind_image("rw_new_locks", &new_locks_tx_);
      sub.dispatch(display_groups);
      sub.barrier(barrier);
    }
    if (sharpen) {
      PassSimple::Sub &sub = ps.sub("RCAS");
      sub.shader_set(shaders.static_shader_get(SH_FSR3_RCAS));
      sub.bind_texture("r_rcas_input", &upscaled_uav_, linear);
      sub.bind_image("rw_upscaled_output", &output_tx_);
      sub.dispatch(rcas_groups);
      sub.barrier(barrier);
    }
  }

  {
    PassSimple &ps = autogen_reactive_ps_;
    ps.init();
    ps.shader_set(shaders.static_shader_get(SH_FSR3_AUTOGEN_REACTIVE));
    ps.bind_ubo("fsr3_buf", data_);
    ps.bind_texture("r_input_opaque_only", &opaque_tx_, linear);
    ps.bind_texture("r_input_color_jittered", &autogen_color_tx_, linear);
    ps.bind_image("rw_output_autoreactive", &autoreactive_tx_);
    ps.dispatch(render_groups);
    ps.barrier(GPU_BARRIER_TEXTURE_FETCH);
  }
}

void UpscaleModule::constants_update(bool reset)
{
  FSR3UpscalerData &d = data_;

  d.previous_frame_jitter = d.jitter;
  d.jitter = jitter_px_;
  d.previous_frame_render_size = d.render_size;
  d.render_size = render_res_;
  d.max_render_size = render_res_;
  d.previous_frame_upscale_size = d.upscale_size;
  d.upscale_size = display_res_;
  d.max_upscale_size = display_res_;
  d.downscale_factor = float2(render_res_) / float2(display_res_);

  /* Field of view from the unjittered projection: winmat[0][0] and winmat[1][1] are the
   * cotangents of the half angles. FidelityFX derives them from the vertical angle and the
   * aspect ratio, the same for square pixels. It has no orthographic support: its depth
   * reconstruction assumes a perspective camera. */
  const float4x4 &winmat = inst_->uniform_data.projectionmat;
  d.tan_half_fov = 1.0f / winmat[0][0];

  /* setupDeviceDepthToViewSpaceDepthParams(), reversed and finite. Blender's reversed depth is
   * the D3D one, d = q * far / z - q with q = near / (far - near): z = q * far / (d + q). */
  /* The clip distances of the frame's projection: the scene camera's are only synced for F12. */
  const float near = inst_->uniform_data.z_near;
  const float far = inst_->uniform_data.z_far;
  const float q = near / (far - near);
  d.device_to_view_depth = float4(-q, q * far, 1.0f / winmat[0][0], 1.0f / winmat[1][1]);

  d.view_space_to_meters_factor = (inst_->scene != nullptr && inst_->scene->unit.scale_length > 0.0f) ?
                                      inst_->scene->unit.scale_length :
                                      1.0f;
  /* No pre-exposure: the color is scene linear. 0 means 1 (FidelityFX's default texture). */
  d.delta_pre_exposure = 1.0f;
  d.exposure = 0.0f;

  /* EEVEE's motion vectors: previous minus current UV, packed * 0.01 (velocity_pack). */
  d.motion_vector_scale = float2(100.0f);
  d.motion_vector_jitter_cancellation = float2(0.0f);

  d.jitter_sequence_length = float(jitter_phase_count_);
  d.delta_time = math::clamp(inst_->delta_time_ms / 1000.0f, 0.0f, 1.0f);
  d.frame_index = reset ? 0.0f : d.frame_index + 1.0f;

  d.velocity_factor = 1.0f;
  d.reactiveness_scale = 1.0f;
  d.shading_change_scale = 1.0f;
  d.accumulation_added_per_frame = 1.0f / 3.0f;
  d.min_disocclusion_accumulation = -1.0f / 3.0f;

  /* ffxSpdSetup(): mips from the largest side, at most 12. */
  d.spd_mips = uint(math::min(std::floor(std::log2(float(math::max(render_res_.x, render_res_.y)))),
                              12.0f));
  d.spd_num_work_groups = uint(spd_group_count_.x * spd_group_count_.y);
  d.spd_work_group_offset = uint2(0);
  d.spd_render_size = uint2(render_res_);

  /* FsrRcasCon(): sharpness in stops, from FidelityFX's [0, 1] sharpness. */
  const float sharpness = std::exp2(-(-2.0f * inst_->upscale_settings.sharpness + 2.0f));
  const uint half = math::float_to_half(sharpness);
  d.rcas_config = uint4(float_as_uint(sharpness), half | (half << 16u), 0u, 0u);

  d.gen_reactive_scale = reactive_scale;
  d.gen_reactive_threshold = reactive_threshold;
  d.gen_reactive_binary_value = reactive_binary_value;
  d.gen_reactive_flags = reactive_flags;
  data_.push_update();
}

void UpscaleModule::generate_masks(gpu::Texture *opaque_tx,
                                   gpu::Texture *combined_tx,
                                   gpu::Texture *reactive_tx,
                                   gpu::Texture * /*transp_tx*/)
{
  if (!is_initialized_) {
    return;
  }
  /* The pass reads render_size, Tonemap() and the cbGenerateReactive values. */
  data_.render_size = render_res_;
  data_.gen_reactive_scale = reactive_scale;
  data_.gen_reactive_threshold = reactive_threshold;
  data_.gen_reactive_binary_value = reactive_binary_value;
  data_.gen_reactive_flags = reactive_flags;
  data_.push_update();
  opaque_tx_ = opaque_tx;
  autogen_color_tx_ = combined_tx;
  autoreactive_tx_ = reactive_tx;
  inst_->manager->submit(autogen_reactive_ps_);
}

void UpscaleModule::apply_fsr3(gpu::Texture *src, gpu::Texture *dst, gpu::Texture * /*ui_tx*/)
{
  if (!is_initialized_) {
    return;
  }
  GPU_debug_group_begin("FSR3");

  const bool reset = camera_cut_pending_ || first_execution_;
  camera_cut_pending_ = false;
  if (first_execution_) {
    for (int i = 0; i < 2; i++) {
      accumulation_tx_[i].clear(float4(0.0f));
      luma_tx_[i].clear(float4(0.0f));
    }
    first_execution_ = false;
  }

  /* Halves of the ping-pong resources: odd frames swap them. */
  const int cur = frame_ & 1;
  const int prev = cur ^ 1;
  accumulation_srv_ = accumulation_tx_[cur];
  accumulation_uav_ = accumulation_tx_[prev];
  upscaled_srv_ = upscaled_tx_[cur];
  upscaled_uav_ = upscaled_tx_[prev];
  luma_history_srv_ = luma_history_tx_[cur];
  luma_history_uav_ = luma_history_tx_[prev];
  luma_current_ = luma_tx_[cur];
  luma_previous_ = luma_tx_[prev];

  color_tx_ = src;
  depth_tx_ = inst_->render_buffers.depth_tx;
  motion_tx_ = inst_->render_buffers.vector_tx;
  reactive_tx_ = inst_->render_buffers.reactive_mask_tx;
  output_tx_ = dst;

  constants_update(reset);

  if (reset) {
    accumulation_tx_[cur].clear(float4(0.0f));
    frame_info_buf_[0] = float4(-1.0f, 1.0f, 0.0f, 0.0f);
    frame_info_buf_.push_update();
  }
  /* Max of the reprojected reversed depths: 0 is the far plane. */
  reconstructed_depth_tx_.clear(uint4(0u));
  spd_atomic_buf_.clear_to_zero();
  inst_->manager->submit(prepare_ps_);
  const bool sharpen = inst_->upscale_settings.sharpness > 0.0f;
  inst_->manager->submit(sharpen ? accumulate_sharpen_ps_ : accumulate_ps_);

  frame_++;
  GPU_debug_group_end();
}

}  // namespace blender::eevee_game
