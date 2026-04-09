/* SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_game_upscaling.hh"
#include "eevee_game_instance.hh"

#include "GPU_context.hh"

/* DNA_scene_types.h for UnitSettings::scale_length */
#include "DNA_scene_types.h"

namespace blender::eevee_game {

#ifdef WITH_AMD_FSR3

/* ================================================================
 * to_ffx_quality()
 *
 * Maps eevee_game UpscaleMode to the FSR3 SDK FfxFsr3QualityMode enum.
 * OFF is never passed here — the caller guards against it.
 *
 * FSR3 quality modes control the render-resolution ratio:
 *   ULTRA_QUALITY : ~77% of display res  (sharpest, most expensive)
 *   QUALITY       : ~67%
 *   BALANCED      : ~59%
 *   PERFORMANCE   : ~50%  (fastest, most visible upscaling artefacts)
 * ================================================================ */

/* static */
FfxFsr3QualityMode UpscaleModule::to_ffx_quality(UpscaleMode mode)
{
  switch (mode) {
    case UpscaleMode::FSR3_ULTRA_QUALITY: return FFX_FSR3_QUALITY_MODE_ULTRA_QUALITY;
    case UpscaleMode::FSR3_QUALITY:       return FFX_FSR3_QUALITY_MODE_QUALITY;
    case UpscaleMode::FSR3_BALANCED:      return FFX_FSR3_QUALITY_MODE_BALANCED;
    case UpscaleMode::FSR3_PERFORMANCE:   return FFX_FSR3_QUALITY_MODE_PERFORMANCE;
    default:
      BLI_assert_unreachable();
      return FFX_FSR3_QUALITY_MODE_QUALITY;
  }
}

/* ================================================================
 * get_command_list()
 *
 * Returns the currently active Vulkan command buffer wrapped as an
 * FfxCommandList so the FSR3 SDK can record its compute dispatches
 * into the same command stream as the rest of the frame.
 *
 * GPU_vk_command_buffer_get() is the public API that gives us the
 * VkCommandBuffer without touching Blender GPU internals directly.
 * The cast chain mirrors the pattern used in init() for VkDevice. */
FfxCommandList UpscaleModule::get_command_list()
{
  const GPUVKDeviceHandles handles = GPU_vk_device_handles_get();
  VkCommandBuffer cmd = reinterpret_cast<VkCommandBuffer>(
      static_cast<uintptr_t>(handles.vk_command_buffer));
  return ffxGetCommandListVK(cmd);
}

/* ================================================================
 * bridge_texture()
 *
 * Wraps a gpu::Texture* into an FfxResource so FSR3 can read or write
 * it within its Vulkan dispatch passes.
 *
 * The public GPU_texture_vk_handles_get() gives us the VkImage,
 * VkImageView, and format without requiring access to Blender's
 * internal VKTexture class.  ffxGetTextureResourceVK() then wraps
 * these into an FfxResource with the correct state and debug label.
 *
 * FfxResourceDescription is filled from the gpu::Texture dimensions
 * and format so FSR3 can reason about mip counts and array layers
 * without deriving them from Vulkan itself. */
FfxResource UpscaleModule::bridge_texture(gpu::Texture *tx,
                                           const wchar_t *debug_name,
                                           FfxResourceStates initial_state)
{
  BLI_assert(tx != nullptr);

  const GPUTextureVKHandles vk = GPU_texture_vk_handles_get(tx->get());
  const int3 size = tx->size();

  FfxResourceDescription desc = {};
  desc.type   = FFX_RESOURCE_TYPE_TEXTURE2D;
  desc.format = ffxGetSurfaceFormatVK(static_cast<VkFormat>(vk.vk_format));
  desc.width  = uint32_t(size.x);
  desc.height = uint32_t(size.y);
  desc.depth  = 1;
  desc.mipCount = uint32_t(tx->mip_count());
  desc.flags  = FFX_RESOURCE_FLAGS_NONE;
  desc.usage  = (initial_state == FFX_RESOURCE_STATE_UNORDERED_ACCESS) ?
                    FFX_RESOURCE_USAGE_UAV :
                    FFX_RESOURCE_USAGE_READ_ONLY;

  return ffxGetTextureResourceVK(
      &fsr3_context_,
      reinterpret_cast<VkImage>(static_cast<uintptr_t>(vk.vk_image)),
      reinterpret_cast<VkImageView>(static_cast<uintptr_t>(vk.vk_image_view)),
      uint32_t(size.x),
      uint32_t(size.y),
      static_cast<VkFormat>(vk.vk_format),
      debug_name,
      initial_state);
}

#endif /* WITH_AMD_FSR3 */

/* ================================================================
 * Destructor
 * ================================================================ */

UpscaleModule::~UpscaleModule()
{
#ifdef WITH_AMD_FSR3
  if (is_initialized_) {
    /* GPU must be idle before destroying FSR resources. */
    ffxFsr3ContextDestroy(&fsr3_context_);
    is_initialized_ = false;
  }
#endif
}

/* ================================================================
 * init()
 * ================================================================ */

void UpscaleModule::init(int2 render_res, int2 display_res)
{
#ifdef WITH_AMD_FSR3
  /* FSR3 is Vulkan-only. If the active GPU backend is OpenGL or Metal,
   * GPU_vk_device_handles_get() returns zeroed handles and the subsequent
   * ffxGetScratchMemorySizeVK() call will crash or produce nonsense.
   * Mark as uninitialised and return cleanly; ShadingView::render() will
   * fall back to SMAA/FXAA automatically because is_initialized_ == false. */
  if (GPU_backend_get_type() != GPU_BACKEND_VULKAN) {
    is_initialized_ = false;
    return;
  }

  if (is_initialized_) {
    ffxFsr3ContextDestroy(&fsr3_context_);
    is_initialized_ = false;
  }

  render_res_  = render_res;
  display_res_ = display_res;

  /* Retrieve Vulkan device handles via the public GPU API. */
  const GPUVKDeviceHandles dev_handles = GPU_vk_device_handles_get();

  const VkPhysicalDevice vk_phys_dev = reinterpret_cast<VkPhysicalDevice>(
      static_cast<uintptr_t>(dev_handles.vk_physical_device));
  const VkDevice vk_device = reinterpret_cast<VkDevice>(
      static_cast<uintptr_t>(dev_handles.vk_device));

  const size_t sz_shared  = ffxGetScratchMemorySizeVK(vk_phys_dev, FFX_FSR3_CONTEXT_COUNT);
  const size_t sz_upscale = ffxGetScratchMemorySizeVK(vk_phys_dev, FFX_FSR3UPSCALER_CONTEXT_COUNT);
  const size_t sz_fi      = ffxGetScratchMemorySizeVK(vk_phys_dev, FFX_FRAMEINTERPOLATION_CONTEXT_COUNT);

  scratch_shared_.resize(sz_shared,   0);
  scratch_upscale_.resize(sz_upscale, 0);
  scratch_fi_.resize(sz_fi,           0);

  const FfxDevice ffx_device = ffxGetDeviceVK(vk_device);
  FfxErrorCode err;

  err = ffxGetInterfaceVK(&context_desc_.backendInterfaceSharedResources,
                           ffx_device,
                           scratch_shared_.data(),  scratch_shared_.size(),
                           FFX_FSR3_CONTEXT_COUNT);
  BLI_assert_msg(err == FFX_OK, "FSR3: failed to init shared resources backend");

  err = ffxGetInterfaceVK(&context_desc_.backendInterfaceUpscaling,
                           ffx_device,
                           scratch_upscale_.data(), scratch_upscale_.size(),
                           FFX_FSR3UPSCALER_CONTEXT_COUNT);
  BLI_assert_msg(err == FFX_OK, "FSR3: failed to init upscaling backend");

  err = ffxGetInterfaceVK(&context_desc_.backendInterfaceFrameInterpolation,
                           ffx_device,
                           scratch_fi_.data(),      scratch_fi_.size(),
                           FFX_FRAMEINTERPOLATION_CONTEXT_COUNT);
  BLI_assert_msg(err == FFX_OK, "FSR3: failed to init frame interpolation backend");

  context_desc_.flags = FFX_FSR3_ENABLE_HIGH_DYNAMIC_RANGE |
                        FFX_FSR3_ENABLE_UPSCALING_ONLY;

  context_desc_.maxRenderSize  = {uint32_t(render_res.x),  uint32_t(render_res.y)};
  context_desc_.maxUpscaleSize = {uint32_t(display_res.x), uint32_t(display_res.y)};
  context_desc_.displaySize    = {uint32_t(display_res.x), uint32_t(display_res.y)};
  context_desc_.backBufferFormat = FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;

#ifndef NDEBUG
  context_desc_.fpMessage = [](FfxMsgType type, const wchar_t *msg) {
    if (type == FFX_MESSAGE_TYPE_ERROR) {
      fprintf(stderr, "[FSR3 ERROR] %ls\n", msg);
    }
  };
#endif

  err = ffxFsr3ContextCreate(&fsr3_context_, &context_desc_);
  BLI_assert_msg(err == FFX_OK, "FSR3: ffxFsr3ContextCreate failed");
  is_initialized_ = (err == FFX_OK);

  if (!is_initialized_) {
    return;
  }

  jitter_phase_count_ = ffxFsr3GetJitterPhaseCount(render_res.x, display_res.x);
  jitter_frame_index_ = 0;

#else
  (void)render_res;
  (void)display_res;
#endif /* WITH_AMD_FSR3 */
}

/* ================================================================
 * apply_fsr3()
 * ================================================================ */

float2 UpscaleModule::advance_jitter()
{
#ifdef WITH_AMD_FSR3
  if (!is_initialized_) {
    return float2(0.0f);
  }
  float jitter_x = 0.0f, jitter_y = 0.0f;
  ffxFsr3GetJitterOffset(&jitter_x, &jitter_y, jitter_frame_index_, jitter_phase_count_);
  jitter_frame_index_ = (jitter_frame_index_ + 1) % jitter_phase_count_;
  /* Convert from pixel-space SDK offsets to NDC UV fraction expected by
   * the projection matrix offset (see begin_sync usage below). */
  const int2 render_res = inst_->film.render_extent_get();
  return float2(2.0f * jitter_x / float(render_res.x),
               -2.0f * jitter_y / float(render_res.y));
#else
  return float2(0.0f);
#endif
}

void UpscaleModule::apply_fsr3(gpu::Texture *src, gpu::Texture *dst, gpu::Texture *ui_tx)
{
#ifdef WITH_AMD_FSR3
  if (!is_initialized_) {
    return;
  }

  /* Jitter was already advanced in begin_sync() via advance_jitter().
   * inst_->uniform_data.jitter is already set; read it back for the SDK. */
  const int2 render_res = inst_->film.render_extent_get();
  const float jitter_x = inst_->uniform_data.jitter.x * float(render_res.x) / 2.0f;
  const float jitter_y = -inst_->uniform_data.jitter.y * float(render_res.y) / 2.0f;

  FfxFsr3DispatchUpscaleDescription dispatch = {};
  dispatch.commandList = get_command_list();

  dispatch.color = bridge_texture(src,
      L"FSR3_Color",        FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  dispatch.depth = bridge_texture(&inst_->render_buffers.depth_tx,
      L"FSR3_Depth",        FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  dispatch.motionVectors = bridge_texture(inst_->render_buffers.vector_tx.get(),
      L"FSR3_MotionVectors", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  dispatch.reactive = bridge_texture(inst_->render_buffers.reactive_mask_tx.get(),
      L"FSR3_Reactive",     FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  dispatch.transparencyAndComposition = bridge_texture(
      inst_->render_buffers.transp_mask_tx.get(),
      L"FSR3_TransComp",    FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
  dispatch.upscaleOutput = bridge_texture(dst,
      L"FSR3_Output",       FFX_RESOURCE_STATE_UNORDERED_ACCESS);

  dispatch.jitterOffset      = {jitter_x, jitter_y};
  dispatch.motionVectorScale = {float(render_res.x), float(render_res.y)};
  dispatch.renderSize        = {uint32_t(render_res.x),   uint32_t(render_res.y)};
  dispatch.upscaleSize       = {uint32_t(display_res_.x),  uint32_t(display_res_.y)};
  dispatch.frameTimeDelta    = inst_->delta_time_ms;
  dispatch.preExposure       = 1.0f;

  const CameraData &cam = inst_->camera.data_get();
  const float2 render_res_f = float2(render_res);
  /* Same guard as in begin_sync(): during window resize render_res.y can
   * transiently be 0. A NaN cameraFovAngleVertical causes FSR3 to assert
   * or produce garbage temporal accumulation for the entire subsequent
   * sequence until the context is recreated. */
  const float  aspect       = (render_res_f.y > 0.0f) ?
                               render_res_f.x / render_res_f.y : 1.0f;

  const float sensor_height = cam.sensor_width / aspect;
  dispatch.cameraFovAngleVertical = 2.0f * atanf(sensor_height / (2.0f * cam.focal_length));

  dispatch.cameraNear = cam.clip_near;
  dispatch.cameraFar  = cam.clip_far;

  /* FIX: was hardcoded to 1.0f ("Blender scene unit = 1 metre").
   * That assumption breaks when the user has changed the scene unit scale (e.g. cm, mm scale).
   * scene->unit.scale_length is the metres-per-Blender-unit factor set in the scene properties.
   * Passing the correct value prevents FSR3 from computing wrong motion vector magnitudes,
   * which would cause ghosting / smearing artefacts on fast-moving objects. */
  dispatch.viewSpaceToMetersFactor = (inst_->scene != nullptr) ?
      inst_->scene->unit.scale_length :
      1.0f; /* Fallback to 1 m/unit if scene is unavailable (should never happen in game mode). */

  dispatch.reset            = camera_cut_pending_;
  camera_cut_pending_       = false;
  dispatch.enableSharpening = false;
  dispatch.sharpness        = 0.0f;

  const FfxErrorCode err = ffxFsr3ContextDispatchUpscale(&fsr3_context_, &dispatch);
  BLI_assert_msg(err == FFX_OK, "FSR3: ffxFsr3ContextDispatchUpscale failed");

  if (ui_tx != nullptr) {
    GPU_texture_copy(dst, ui_tx);
  }

#else
  (void)src;
  (void)dst;
  (void)ui_tx;
#endif /* WITH_AMD_FSR3 */
}

/* ================================================================
 * generate_masks()
 * ================================================================ */

void UpscaleModule::generate_masks(gpu::Texture *opaque_tx,
                                   gpu::Texture *combined_tx,
                                   gpu::Texture *reactive_tx,
                                   gpu::Texture *transp_tx)
{
#ifdef WITH_AMD_FSR3
  if (!is_initialized_) {
    return;
  }

  const int2 render_res = inst_->film.render_extent_get();
  const FfxDimensions2D ffx_res = {uint32_t(render_res.x), uint32_t(render_res.y)};

  {
    FfxFsr3GenerateReactiveDescription desc = {};
    desc.commandList     = get_command_list();
    desc.colorOpaqueOnly = bridge_texture(opaque_tx,
        L"FSR3_OpaqueOnly",  FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    desc.colorPreUpscale = bridge_texture(combined_tx,
        L"FSR3_Combined",    FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    desc.outReactive     = bridge_texture(reactive_tx,
        L"FSR3_Reactive",    FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    desc.renderSize      = ffx_res;
    desc.scale           = 1.0f;
    desc.cutoffThreshold = 0.1f;
    desc.binaryValue     = 1.0f;
    desc.flags           = 0;
    const FfxErrorCode err = ffxFsr3ContextGenerateReactiveMask(&fsr3_context_, &desc);
    BLI_assert_msg(err == FFX_OK, "FSR3: GenerateReactiveMask failed");
  }

  {
    FfxFsr3GenerateReactiveDescription desc = {};
    desc.commandList     = get_command_list();
    desc.colorOpaqueOnly = bridge_texture(opaque_tx,
        L"FSR3_OpaqueOnly2", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    desc.colorPreUpscale = bridge_texture(combined_tx,
        L"FSR3_Combined2",   FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    desc.outReactive     = bridge_texture(transp_tx,
        L"FSR3_TransComp",   FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    desc.renderSize      = ffx_res;
    desc.scale           = 0.5f;
    desc.cutoffThreshold = 0.05f;
    desc.binaryValue     = 0.5f;
    desc.flags           = 0;
    const FfxErrorCode err = ffxFsr3ContextGenerateReactiveMask(&fsr3_context_, &desc);
    BLI_assert_msg(err == FFX_OK, "FSR3: GenerateReactiveMask (transp) failed");
  }

#else
  (void)opaque_tx;
  (void)combined_tx;
  (void)reactive_tx;
  (void)transp_tx;
#endif /* WITH_AMD_FSR3 */
}

/* ================================================================
 * calculate_render_res() — static
 * ================================================================ */

/* static */
int2 UpscaleModule::calculate_render_res(int2 display_res, UpscaleMode mode)
{
#ifdef WITH_AMD_FSR3
  if (mode == UpscaleMode::OFF) {
    return display_res;
  }
  uint32_t rw, rh;
  ffxFsr3GetRenderResolutionFromQualityMode(
      &rw, &rh,
      uint32_t(display_res.x), uint32_t(display_res.y),
      to_ffx_quality(mode));
  return int2(int(rw), int(rh));
#else
  (void)mode;
  return display_res;
#endif
}

} // namespace blender::eevee_game
