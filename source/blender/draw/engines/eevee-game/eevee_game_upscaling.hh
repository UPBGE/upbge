/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DRW_gpu_wrapper.hh"
#include "draw_pass.hh"

#include "eevee_game_defines.hh"
#include "eevee_game_fsr3_shared.hh"

namespace blender::eevee_game {

class GameInstance;

/**
 * UpscaleModule: FSR 3.1 temporal upscaling on every GPU backend (OpenGL, Vulkan, Metal).
 *
 * The passes are FidelityFX's upscaler shaders (SDK v1.1.4), vendored in `shaders/fsr3/` and
 * dispatched as our own compute passes; this file is the port of the host side
 * (ffx_fsr3upscaler.cpp): resources, constants and schedule.
 *
 * Call order per frame:
 *   1. advance_jitter()   begin_sync(), before the projection matrix is built
 *   2. generate_masks()   after the transparent pass
 *   3. apply_fsr3()       final upscale
 */
class UpscaleModule {
 public:
  UpscaleModule(GameInstance &inst) : inst_(&inst) {}

  /** (Re)create the resources for a render and display resolution pair. */
  void init(int2 render_res, int2 display_res);

  /**
   * Next jitter of the Halton(2,3) sequence, as the NDC offset of the projection matrix.
   * The projection shift moves the image by +jitter pixels: render texel i samples the scene at
   * i + 0.5 - jitter. Texel row 0 is NDC y = -1 on every backend, no sign flip on y.
   */
  float2 advance_jitter();

  /**
   * Reactive mask from the color before and after the transparent surfaces (FidelityFX's
   * ffxFsr3UpscalerContextGenerateReactiveMask). \a transp_tx is not written: FSR 3.1 has no
   * automatic transparency and composition mask, apply_fsr3() binds an empty one.
   */
  void generate_masks(gpu::Texture *opaque_tx,
                      gpu::Texture *combined_tx,
                      gpu::Texture *reactive_tx,
                      gpu::Texture *transp_tx);

  /**
   * Upscale \a src (scene linear, render resolution) into \a dst (display resolution,
   * RGBA16F). \a ui_tx is unused: the UI is composited after upscaling.
   */
  void apply_fsr3(gpu::Texture *src, gpu::Texture *dst, gpu::Texture *ui_tx);

  /** Render resolution of a mode: FidelityFX's ratios (Ultra Quality is FSR 2.0's 1.3). */
  static int2 calculate_render_res(int2 display_res, UpscaleMode mode);

  bool is_initialized() const
  {
    return is_initialized_;
  }

  /** Discard the history on the next frame (camera cut, teleport, scene load). */
  void notify_camera_cut()
  {
    camera_cut_pending_ = true;
  }

  /* ffxFsr3UpscalerGenerateReactiveDescription, FidelityFX sample defaults. */
  float reactive_scale = 1.0f;
  float reactive_threshold = 0.2f;
  float reactive_binary_value = 0.9f;
  uint reactive_flags = FSR3_AUTOREACTIVE_APPLY_TONEMAP | FSR3_AUTOREACTIVE_APPLY_THRESHOLD |
                        FSR3_AUTOREACTIVE_USE_COMPONENTS_MAX;

 private:
  void passes_sync();
  void constants_update(bool reset);

  GameInstance *inst_;
  bool is_initialized_ = false;
  bool camera_cut_pending_ = false;
  bool first_execution_ = true;
  int2 render_res_ = int2(0);
  int2 display_res_ = int2(0);
  /* Dispatches since init(): parity selects the halves of the ping-pong resources. */
  uint frame_ = 0;
  int jitter_phase_count_ = 0;
  int jitter_index_ = 0;
  float2 jitter_px_ = float2(0.0f);
  float2 previous_jitter_px_ = float2(0.0f);
  int2 spd_group_count_ = int2(0);

  draw::UniformBuffer<FSR3UpscalerData> data_ = {"fsr3_buf"};
  /* SPD work-group counter (FidelityFX: 1x1 R32UI image). */
  draw::StorageArrayBuffer<uint, 4, true> spd_atomic_buf_ = {"fsr3_spd_atomic"};
  /* Exposure, average log luma, average luma (FidelityFX: 1x1 RGBA32F image). */
  draw::StorageArrayBuffer<float4, 1> frame_info_buf_ = {"fsr3_frame_info"};

  /* Persistent: read by the next frame. Pairs alternate between current and previous. */
  draw::Texture accumulation_tx_[2] = {{"fsr3_accumulation_1"}, {"fsr3_accumulation_2"}};
  draw::Texture luma_tx_[2] = {{"fsr3_luma_1"}, {"fsr3_luma_2"}};
  draw::Texture luma_history_tx_[2] = {{"fsr3_luma_history_1"}, {"fsr3_luma_history_2"}};
  draw::Texture upscaled_tx_[2] = {{"fsr3_upscaled_1"}, {"fsr3_upscaled_2"}};
  /* Written by prepare_reactivity, read then cleared by accumulate. */
  draw::Texture new_locks_tx_ = {"fsr3_new_locks"};
  /* Frame local. The farthest depth, then the luma instability (FidelityFX aliases them). */
  draw::Texture intermediate_tx_ = {"fsr3_intermediate"};
  draw::Texture shading_change_tx_ = {"fsr3_shading_change"};
  draw::Texture spd_mips_tx_ = {"fsr3_spd_mips"};
  draw::Texture farthest_depth_mip1_tx_ = {"fsr3_farthest_depth_mip1"};
  draw::Texture dilated_reactive_tx_ = {"fsr3_dilated_reactive_masks"};
  draw::Texture dilated_motion_tx_ = {"fsr3_dilated_motion_vectors"};
  draw::Texture dilated_depth_tx_ = {"fsr3_dilated_depth"};
  draw::Texture reconstructed_depth_tx_ = {"fsr3_reconstructed_previous_nearest_depth"};
  /* 1x1 zero: no transparency and composition mask. */
  draw::Texture empty_mask_tx_ = {"fsr3_empty_mask"};

  /* Bound by reference in the passes, updated every frame. */
  gpu::Texture *color_tx_ = nullptr;
  gpu::Texture *depth_tx_ = nullptr;
  gpu::Texture *motion_tx_ = nullptr;
  gpu::Texture *reactive_tx_ = nullptr;
  gpu::Texture *output_tx_ = nullptr;
  gpu::Texture *opaque_tx_ = nullptr;
  gpu::Texture *autogen_color_tx_ = nullptr;
  gpu::Texture *autoreactive_tx_ = nullptr;
  gpu::Texture *accumulation_srv_ = nullptr;
  gpu::Texture *accumulation_uav_ = nullptr;
  gpu::Texture *luma_current_ = nullptr;
  gpu::Texture *luma_previous_ = nullptr;
  gpu::Texture *luma_history_srv_ = nullptr;
  gpu::Texture *luma_history_uav_ = nullptr;
  gpu::Texture *upscaled_srv_ = nullptr;
  gpu::Texture *upscaled_uav_ = nullptr;
  gpu::Texture *spd_mip_[6] = {};

  /* Prepare inputs to luma instability, then the accumulation with or without RCAS. */
  draw::PassSimple prepare_ps_ = {"FSR3.Prepare"};
  draw::PassSimple accumulate_ps_ = {"FSR3.Accumulate"};
  draw::PassSimple accumulate_sharpen_ps_ = {"FSR3.AccumulateSharpen"};
  draw::PassSimple autogen_reactive_ps_ = {"FSR3.AutogenReactive"};
};

}  // namespace blender::eevee_game
