/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Resource access of the FSR 3.1 upscaler passes: Blender port of FidelityFX's
 * ffx_fsr3upscaler_callbacks_glsl.h (SDK v1.1.4). Same functions, same guards
 * (FSR3UPSCALER_BIND_*, set by each pass create-info) and same resource names; the resources
 * are declared by the create-infos instead of `layout(set, binding)`.
 *
 * Differences with FidelityFX:
 * - One uniform buffer (fsr3_buf) holds the four constant buffers.
 * - The exposure comes from fsr3_buf instead of a 1x1 texture.
 * - Frame info (1x1 RGBA32F) and the SPD atomic counter (1x1 R32UI) are storage buffers: the
 *   luma pyramid then binds 7 images (OpenGL guarantees 8, FidelityFX binds 9).
 * - Samplers are combined: every texture is bound with linear filtering and clamp, which is
 *   what FidelityFX's Sample* functions use (texelFetch ignores the sampler).
 * - rw_luma_history is RGBA16F, the format of the resource (FidelityFX's GLSL declares rgba8).
 * - The automatic reactive mask is 8 bits (FidelityFX: r32f).
 */

#pragma once

#include "infos/eevee_game_fsr3_infos.hh"

SHADER_LIBRARY_CREATE_INFO(eevee_game_fsr3_common)

int2 RenderSize()
{
  return fsr3_buf.render_size;
}

int2 PreviousFrameRenderSize()
{
  return fsr3_buf.previous_frame_render_size;
}

int2 MaxRenderSize()
{
  return fsr3_buf.max_render_size;
}

int2 UpscaleSize()
{
  return fsr3_buf.upscale_size;
}

int2 PreviousFrameUpscaleSize()
{
  return fsr3_buf.previous_frame_upscale_size;
}

int2 MaxUpscaleSize()
{
  return fsr3_buf.max_upscale_size;
}

float2 Jitter()
{
  return fsr3_buf.jitter;
}

float2 PreviousFrameJitter()
{
  return fsr3_buf.previous_frame_jitter;
}

float4 DeviceToViewSpaceTransformFactors()
{
  return fsr3_buf.device_to_view_depth;
}

float2 MotionVectorScale()
{
  return fsr3_buf.motion_vector_scale;
}

float2 DownscaleFactor()
{
  return fsr3_buf.downscale_factor;
}

float2 MotionVectorJitterCancellation()
{
  return fsr3_buf.motion_vector_jitter_cancellation;
}

float TanHalfFoV()
{
  return fsr3_buf.tan_half_fov;
}

float JitterSequenceLength()
{
  return fsr3_buf.jitter_sequence_length;
}

float DeltaTime()
{
  return fsr3_buf.delta_time;
}

float DeltaPreExposure()
{
  return fsr3_buf.delta_pre_exposure;
}

float ViewSpaceToMetersFactor()
{
  return fsr3_buf.view_space_to_meters_factor;
}

float FrameIndex()
{
  return fsr3_buf.frame_index;
}

float VelocityFactor()
{
  return fsr3_buf.velocity_factor;
}

float AccumulationAddedPerFrame()
{
  return fsr3_buf.accumulation_added_per_frame;
}

float MinDisocclusionAccumulation()
{
  return fsr3_buf.min_disocclusion_accumulation;
}

uint4 RCASConfig()
{
  return fsr3_buf.rcas_config;
}

float GenReactiveScale()
{
  return fsr3_buf.gen_reactive_scale;
}

float GenReactiveThreshold()
{
  return fsr3_buf.gen_reactive_threshold;
}

float GenReactiveBinaryValue()
{
  return fsr3_buf.gen_reactive_binary_value;
}

uint GenReactiveFlags()
{
  return fsr3_buf.gen_reactive_flags;
}

uint MipCount()
{
  return fsr3_buf.spd_mips;
}

uint NumWorkGroups()
{
  return fsr3_buf.spd_num_work_groups;
}

uint2 WorkGroupOffset()
{
  return fsr3_buf.spd_work_group_offset;
}

uint2 SPD_RenderSize()
{
  return fsr3_buf.spd_render_size;
}

float Exposure()
{
  float exposure = fsr3_buf.exposure;
  if (exposure == 0.0f) {
    exposure = 1.0f;
  }
  return exposure;
}

#if defined(FSR3UPSCALER_BIND_SRV_SPD_MIPS)
int2 GetSPDMipDimensions(uint uMipLevel)
{
  return textureSize(r_spd_mips, int(uMipLevel)).xy;
}

float2 SampleSPDMipLevel(float2 fUV, uint mipLevel)
{
  return textureLod(r_spd_mips, fUV, float(mipLevel)).rg;
}
#endif

#if defined(FSR3UPSCALER_BIND_SRV_INPUT_DEPTH)
float LoadInputDepth(int2 iPxPos)
{
  return texelFetch(r_input_depth, iPxPos, 0).r;
}

float SampleInputDepth(float2 fUV)
{
  return textureLod(r_input_depth, fUV, 0.0f).x;
}
#endif

#if defined(FSR3UPSCALER_BIND_SRV_REACTIVE_MASK)
float LoadReactiveMask(int2 iPxPos)
{
  return texelFetch(r_reactive_mask, iPxPos, 0).r * fsr3_buf.reactiveness_scale;
}

int2 GetReactiveMaskResourceDimensions()
{
  return textureSize(r_reactive_mask, 0).xy;
}

float SampleReactiveMask(float2 fUV)
{
  return textureLod(r_reactive_mask, fUV, 0.0f).x * fsr3_buf.reactiveness_scale;
}
#endif

#if defined(FSR3UPSCALER_BIND_SRV_TRANSPARENCY_AND_COMPOSITION_MASK)
float LoadTransparencyAndCompositionMask(uint2 iPxPos)
{
  return texelFetch(r_transparency_and_composition_mask, int2(iPxPos), 0).r;
}

int2 GetTransparencyAndCompositionMaskResourceDimensions()
{
  return textureSize(r_transparency_and_composition_mask, 0).xy;
}

float SampleTransparencyAndCompositionMask(float2 fUV)
{
  return textureLod(r_transparency_and_composition_mask, fUV, 0.0f).x;
}
#endif

#if defined(FSR3UPSCALER_BIND_SRV_INPUT_COLOR)
float3 LoadInputColor(int2 iPxPos)
{
  return texelFetch(r_input_color_jittered, iPxPos, 0).rgb;
}

float3 SampleInputColor(float2 fUV)
{
  return textureLod(r_input_color_jittered, fUV, 0.0f).rgb;
}
#endif

#if defined(FSR3UPSCALER_BIND_SRV_INPUT_MOTION_VECTORS)
float2 LoadInputMotionVector(int2 iPxDilatedMotionVectorPos)
{
  float2 fSrcMotionVector = texelFetch(r_input_motion_vectors, iPxDilatedMotionVectorPos, 0).xy;

  /* EEVEE's motion vectors do not contain the jitter: no MotionVectorJitterCancellation(). */
  float2 fUvMotionVector = fSrcMotionVector * MotionVectorScale();

  return fUvMotionVector;
}
#endif

#if defined(FSR3UPSCALER_BIND_SRV_INTERNAL_UPSCALED)
float4 LoadHistory(int2 iPxHistory)
{
  return texelFetch(r_internal_upscaled_color, iPxHistory, 0);
}

float4 SampleHistory(float2 fUV)
{
  return textureLod(r_internal_upscaled_color, fUV, 0.0f);
}
#endif

#if defined(FSR3UPSCALER_BIND_UAV_LUMA_HISTORY)
void StoreLumaHistory(int2 iPxPos, float4 fLumaHistory)
{
  imageStore(rw_luma_history, iPxPos, fLumaHistory);
}
#endif

#if defined(FSR3UPSCALER_BIND_SRV_LUMA_HISTORY)
float4 LoadLumaHistory(int2 iPxPos)
{
  return texelFetch(r_luma_history, iPxPos, 0);
}

float4 SampleLumaHistory(float2 fUV)
{
  return textureLod(r_luma_history, fUV, 0.0f);
}
#endif

#if defined(FSR3UPSCALER_BIND_SRV_RCAS_INPUT)
float4 LoadRCAS_Input(int2 iPxPos)
{
  return texelFetch(r_rcas_input, iPxPos, 0);
}

float3 SampleRCAS_Input(float2 fUV)
{
  return textureLod(r_rcas_input, fUV, 0.0f).rgb;
}
#endif

#if defined(FSR3UPSCALER_BIND_UAV_INTERNAL_UPSCALED)
void StoreReprojectedHistory(int2 iPxHistory, float4 fHistory)
{
  imageStore(rw_internal_upscaled_color, iPxHistory, fHistory);
}

void StoreInternalColorAndWeight(int2 iPxPos, float4 fColorAndWeight)
{
  imageStore(rw_internal_upscaled_color, iPxPos, fColorAndWeight);
}
#endif

#if defined(FSR3UPSCALER_BIND_UAV_UPSCALED_OUTPUT)
void StoreUpscaledOutput(int2 iPxPos, float3 fColor)
{
  imageStore(rw_upscaled_output, iPxPos, float4(fColor, 1.0f));
}
#endif

#if defined(FSR3UPSCALER_BIND_SRV_ACCUMULATION)
float SampleAccumulation(float2 fUV)
{
  return textureLod(r_accumulation, fUV, 0.0f).x;
}
#endif

#if defined(FSR3UPSCALER_BIND_UAV_ACCUMULATION)
void StoreAccumulation(int2 iPxPos, float fAccumulation)
{
  imageStore(rw_accumulation, iPxPos, float4(fAccumulation, 0.0f, 0.0f, 0.0f));
}
#endif

#if defined(FSR3UPSCALER_BIND_SRV_SHADING_CHANGE)
float LoadShadingChange(int2 iPxPos)
{
  return texelFetch(r_shading_change, iPxPos, 0).x * fsr3_buf.shading_change_scale;
}

float SampleShadingChange(float2 fUV)
{
  return textureLod(r_shading_change, fUV, 0.0f).x * fsr3_buf.shading_change_scale;
}
#endif

#if defined(FSR3UPSCALER_BIND_UAV_SHADING_CHANGE)
void StoreShadingChange(int2 iPxPos, float fShadingChange)
{
  imageStore(rw_shading_change, iPxPos, float4(fShadingChange, 0.0f, 0.0f, 0.0f));
}
#endif

#if defined(FSR3UPSCALER_BIND_SRV_FARTHEST_DEPTH)
int2 GetFarthestDepthResourceDimensions()
{
  return textureSize(r_farthest_depth, 0).xy;
}

float LoadFarthestDepth(int2 iPxPos)
{
  return texelFetch(r_farthest_depth, iPxPos, 0).x;
}

float SampleFarthestDepth(float2 fUV)
{
  return textureLod(r_farthest_depth, fUV, 0.0f).x;
}
#endif

#if defined(FSR3UPSCALER_BIND_UAV_FARTHEST_DEPTH)
void StoreFarthestDepth(int2 iPxPos, float fDepth)
{
  imageStore(rw_farthest_depth, iPxPos, float4(fDepth, 0.0f, 0.0f, 0.0f));
}
#endif

#if defined(FSR3UPSCALER_BIND_SRV_FARTHEST_DEPTH_MIP1)
int2 GetFarthestDepthMip1ResourceDimensions()
{
  return textureSize(r_farthest_depth_mip1, 0).xy;
}

float LoadFarthestDepthMip1(int2 iPxPos)
{
  return texelFetch(r_farthest_depth_mip1, iPxPos, 0).x;
}

float SampleFarthestDepthMip1(float2 fUV)
{
  return textureLod(r_farthest_depth_mip1, fUV, 0.0f).x;
}
#endif

#if defined(FSR3UPSCALER_BIND_UAV_FARTHEST_DEPTH_MIP1)
void StoreFarthestDepthMip1(int2 iPxPos, float fDepth)
{
  imageStore(rw_farthest_depth_mip1, iPxPos, float4(fDepth, 0.0f, 0.0f, 0.0f));
}
#endif

#if defined(FSR3UPSCALER_BIND_SRV_CURRENT_LUMA)
float LoadCurrentLuma(int2 iPxPos)
{
  return texelFetch(r_current_luma, iPxPos, 0).r;
}

float SampleCurrentLuma(float2 uv)
{
  return textureLod(r_current_luma, uv, 0.0f).r;
}
#endif

#if defined(FSR3UPSCALER_BIND_UAV_CURRENT_LUMA)
void StoreCurrentLuma(int2 iPxPos, float fLuma)
{
  imageStore(rw_current_luma, iPxPos, float4(fLuma, 0.0f, 0.0f, 0.0f));
}
#endif

#if defined(FSR3UPSCALER_BIND_SRV_LUMA_INSTABILITY)
float SampleLumaInstability(float2 uv)
{
  return textureLod(r_luma_instability, uv, 0.0f).x;
}
#endif

#if defined(FSR3UPSCALER_BIND_UAV_LUMA_INSTABILITY)
void StoreLumaInstability(int2 iPxPos, float fLumaInstability)
{
  imageStore(rw_luma_instability, iPxPos, float4(fLumaInstability, 0.0f, 0.0f, 0.0f));
}
#endif

#if defined(FSR3UPSCALER_BIND_SRV_PREVIOUS_LUMA)
float LoadPreviousLuma(int2 iPxPos)
{
  return texelFetch(r_previous_luma, iPxPos, 0).r;
}

float SamplePreviousLuma(float2 uv)
{
  return textureLod(r_previous_luma, uv, 0.0f).r;
}
#endif

#if defined(FSR3UPSCALER_BIND_SRV_NEW_LOCKS)
float LoadNewLocks(int2 iPxPos)
{
  return texelFetch(r_new_locks, iPxPos, 0).r;
}
#endif

#if defined(FSR3UPSCALER_BIND_UAV_NEW_LOCKS)
float LoadRwNewLocks(int2 iPxPos)
{
  return imageLoad(rw_new_locks, iPxPos).r;
}

void StoreNewLocks(int2 iPxPos, float newLock)
{
  imageStore(rw_new_locks, iPxPos, float4(newLock, 0.0f, 0.0f, 0.0f));
}
#endif

#if defined(FSR3UPSCALER_BIND_SRV_RECONSTRUCTED_PREV_NEAREST_DEPTH)
float LoadReconstructedPrevDepth(int2 iPxPos)
{
  return uintBitsToFloat(texelFetch(r_reconstructed_previous_nearest_depth, iPxPos, 0).r);
}
#endif

#if defined(FSR3UPSCALER_BIND_UAV_RECONSTRUCTED_PREV_NEAREST_DEPTH)
void StoreReconstructedDepth(int2 iPxSample, float fDepth)
{
  uint uDepth = floatBitsToUint(fDepth);

#  if FSR3_OPTION_INVERTED_DEPTH
  imageAtomicMax(rw_reconstructed_previous_nearest_depth, iPxSample, uDepth);
#  else
  /* Min for standard, max for inverted depth. */
  imageAtomicMin(rw_reconstructed_previous_nearest_depth, iPxSample, uDepth);
#  endif
}

void SetReconstructedDepth(int2 iPxSample, uint uValue)
{
  imageStore(rw_reconstructed_previous_nearest_depth, iPxSample, uint4(uValue, 0u, 0u, 0u));
}
#endif

#if defined(FSR3UPSCALER_BIND_UAV_DILATED_DEPTH)
void StoreDilatedDepth(int2 iPxPos, float fDepth)
{
  imageStore(rw_dilated_depth, iPxPos, float4(fDepth, 0.0f, 0.0f, 0.0f));
}
#endif

#if defined(FSR3UPSCALER_BIND_UAV_DILATED_MOTION_VECTORS)
void StoreDilatedMotionVector(int2 iPxPos, float2 fMotionVector)
{
  imageStore(rw_dilated_motion_vectors, iPxPos, float4(fMotionVector, 0.0f, 0.0f));
}
#endif

#if defined(FSR3UPSCALER_BIND_SRV_DILATED_MOTION_VECTORS)
float2 LoadDilatedMotionVector(int2 iPxInput)
{
  return texelFetch(r_dilated_motion_vectors, iPxInput, 0).xy;
}

float2 SampleDilatedMotionVector(float2 fUV)
{
  return textureLod(r_dilated_motion_vectors, fUV, 0.0f).xy;
}
#endif

#if defined(FSR3UPSCALER_BIND_SRV_DILATED_DEPTH)
float LoadDilatedDepth(int2 iPxInput)
{
  return texelFetch(r_dilated_depth, iPxInput, 0).r;
}

float SampleDilatedDepth(float2 fUV)
{
  return textureLod(r_dilated_depth, fUV, 0.0f).r;
}
#endif

float SampleLanczos2Weight(float x)
{
#if defined(FSR3UPSCALER_BIND_SRV_LANCZOS_LUT)
  return textureLod(r_lanczos_lut, float2(x / 2.0f, 0.5f), 0.0f).x;
#else
  return 0.0f;
#endif
}

#if defined(FSR3UPSCALER_BIND_SRV_DILATED_REACTIVE_MASKS)
float4 SampleDilatedReactiveMasks(float2 fUV)
{
  return textureLod(r_dilated_reactive_masks, fUV, 0.0f);
}
#endif

#if defined(FSR3UPSCALER_BIND_UAV_DILATED_REACTIVE_MASKS)
void StoreDilatedReactiveMasks(int2 iPxPos, float4 fDilatedReactiveMasks)
{
  imageStore(rw_dilated_reactive_masks, iPxPos, fDilatedReactiveMasks);
}
#endif

#if defined(FSR3UPSCALER_BIND_SRV_INPUT_OPAQUE_ONLY)
float3 LoadOpaqueOnly(int2 iPxPos)
{
  return texelFetch(r_input_opaque_only, iPxPos, 0).xyz;
}
#endif

#if defined(FSR3UPSCALER_BIND_UAV_AUTOREACTIVE)
void StoreAutoReactive(int2 iPxPos, float fReactive)
{
  imageStore(rw_output_autoreactive, iPxPos, float4(fReactive, 0.0f, 0.0f, 0.0f));
}
#endif

#if defined(FSR3UPSCALER_BIND_UAV_FRAME_INFO)
float4 LoadFrameInfo()
{
  return rw_frame_info[0];
}

void StoreFrameInfo(float4 fInfo)
{
  rw_frame_info[0] = fInfo;
}
#endif

#if defined(FSR3UPSCALER_BIND_SRV_FRAME_INFO)
float4 FrameInfo()
{
  return r_frame_info[0];
}
#endif

#if defined(FSR3UPSCALER_BIND_UAV_SPD_MIPS_LEVEL_0)
float2 RWLoadPyramid(int2 iPxPos, uint index)
{
  /* Only the last work-group reads, the mip the other groups wrote: 5. */
  if (index == 5u) {
    return imageLoad(rw_spd_mip5, iPxPos).xy;
  }
  return float2(0.0f, 0.0f);
}

void StorePyramid(int2 iPxPos, float2 outValue, uint index)
{
  switch (index) {
    case 0u:
      imageStore(rw_spd_mip0, iPxPos, float4(outValue, 0.0f, 0.0f));
      break;
    case 1u:
      imageStore(rw_spd_mip1, iPxPos, float4(outValue, 0.0f, 0.0f));
      break;
    case 2u:
      imageStore(rw_spd_mip2, iPxPos, float4(outValue, 0.0f, 0.0f));
      break;
    case 3u:
      imageStore(rw_spd_mip3, iPxPos, float4(outValue, 0.0f, 0.0f));
      break;
    case 4u:
      imageStore(rw_spd_mip4, iPxPos, float4(outValue, 0.0f, 0.0f));
      break;
    case 5u:
      imageStore(rw_spd_mip5, iPxPos, float4(outValue, 0.0f, 0.0f));
      break;
    default:
      break;
  }
}
#endif

#if defined(FSR3UPSCALER_BIND_UAV_SPD_GLOBAL_ATOMIC)
/* Macros: the counter is a shared variable, which MSL cannot pass by thread reference. */
#  define SPD_IncreaseAtomicCounter(spdCounter) \
    spdCounter = atomicAdd(rw_spd_global_atomic[0], 1u)
#  define SPD_ResetAtomicCounter() rw_spd_global_atomic[0] = 0u
#endif
