/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_game_defines.hh"
#include "GPU_texture.hh"   /* GPUTextureVKHandles, GPUVKDeviceHandles, GPU_backend_get_type() */

#ifdef WITH_AMD_FSR3
/* FSR 3.1 SDK public headers — no Vulkan internals */
#  include <FidelityFX/host/ffx_fsr3.h>
#  include <FidelityFX/host/ffx_fsr3upscaler.h>
/* Vulkan backend functions: ffxGetResourceVK, ffxGetDeviceVK,
 * ffxGetCommandListVK, ffxGetSurfaceFormatVK, ffxGetScratchMemorySizeVK */
#  include <FidelityFX/host/backends/vk/ffx_vk.h>
#endif

namespace blender::eevee_game {

/**
 * UpscaleModule: FSR 3.1 temporal upscaling, Vulkan backend only.
 *
 * On non-Vulkan builds (WITH_AMD_FSR3 not defined) every method is a no-op
 * and calculate_render_res() returns display_res unchanged.
 * ShadingView::render() falls back to SMAA/FXAA in that path.
 *
 * Call order per frame:
 *   1. generate_masks(opaque, combined, reactive, transp)  — after transparent pass
 *   2. apply_fsr3(src, dst, ui)                           — final upscale
 */
class UpscaleModule {
 public:
  UpscaleModule(class GameInstance &inst) : inst_(&inst) {}
  ~UpscaleModule();

  /** Create (or recreate) the FSR3 Vulkan context for the given resolution pair. */
  void init(int2 render_res, int2 display_res);

  /**
   * Dispatch the FSR3 upscale passes for the current frame.
   * @param src    HDR color at render_res (SFLOAT_16_16_16_16, read).
   * @param dst    Output texture at display_res (written by FSR).
   * @param ui_tx  Full-res UI overlay composited post-upscale (may be null).
   */
  void apply_fsr3(gpu::Texture *src, gpu::Texture *dst, gpu::Texture *ui_tx);

  /**
   * Generate reactive + transparency masks via the SDK built-in pass.
   * Must be called after the transparent pass and before apply_fsr3().
   */
  void generate_masks(gpu::Texture *opaque_tx,
                      gpu::Texture *combined_tx,
                      gpu::Texture *reactive_tx,
                      gpu::Texture *transp_tx);

  /**
   * Compute render resolution from display resolution + quality mode.
   * Delegates to ffxFsr3GetRenderResolutionFromQualityMode() when the SDK
   * is present; returns display_res unchanged otherwise.
   */
  static int2 calculate_render_res(int2 display_res, UpscaleMode mode);

  bool is_initialized() const { return is_initialized_; }

  /**
   * Call this when the camera changes discontinuously (camera cut, scene load, teleport).
   * FSR3 will discard its temporal history for the next frame, preventing ghosting.
   * Safe to call even when FSR is disabled (flag is ignored if not initialized).
   */
  void notify_camera_cut() { camera_cut_pending_ = true; }

  /**
   * Advance the SDK jitter counter and return the UV-space offsets for the
   * NEXT frame's projection matrix.  Called from GameInstance::begin_sync()
   * so the prepass and G-Buffer are rendered with the correct sub-pixel shift.
   *
   * AMD FSR3 integration guide §4.2: jitter must be applied to the projection
   * matrix BEFORE the scene is rendered, not after (which is what apply_fsr3()
   * does — too late for the prepass).  We therefore split jitter advancement
   * out of apply_fsr3() and call it at the start of the frame instead.
   */
  float2 advance_jitter();

 private:
#ifdef WITH_AMD_FSR3
  /**
   * Wrap a gpu::Texture* into an FfxResource using only the public GPU API.
   * Calls GPU_texture_vk_handles_get() — declared in GPU_texture.hh — instead
   * of casting directly to VKTexture. No internal Vulkan headers needed here.
   */
  FfxResource bridge_texture(gpu::Texture *tx,
                              const wchar_t *debug_name,
                              FfxResourceStates initial_state);

  /** Return the active VkCommandBuffer wrapped as FfxCommandList. */
  FfxCommandList get_command_list();

  /** Map UpscaleMode to the FSR3 SDK quality enum. */
  static FfxFsr3QualityMode to_ffx_quality(UpscaleMode mode);

  FfxFsr3Context            fsr3_context_ = {};
  FfxFsr3ContextDescription context_desc_ = {};

  /**
   * Scratch memory for the three FfxInterface backends.
   * Sized once in init() by ffxGetScratchMemorySizeVK(). Never reallocated per frame.
   */
  Vector<uint8_t> scratch_shared_;
  Vector<uint8_t> scratch_upscale_;
  Vector<uint8_t> scratch_fi_;

  /** Jitter state driven by the SDK's own Halton sequence. */
  int32_t jitter_phase_count_ = 0;
  int32_t jitter_frame_index_ = 0;
#endif /* WITH_AMD_FSR3 */

  GameInstance *inst_;
  bool          is_initialized_    = false;
  bool          camera_cut_pending_ = false; /* Set by notify_camera_cut(), consumed in apply_fsr3() */
  int2          render_res_        = int2(0);
  int2          display_res_       = int2(0);
};

} // namespace blender::eevee_game
