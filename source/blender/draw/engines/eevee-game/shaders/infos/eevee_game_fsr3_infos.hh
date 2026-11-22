/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * FSR 3.1 upscaler passes (FidelityFX SDK v1.1.4), in dispatch order. Resource names and the
 * FSR3UPSCALER_BIND_* guards are FidelityFX's: they select the accessors of
 * eevee_game_fsr3_callbacks_lib.glsl. Every texture is bound with linear filtering and clamp.
 */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"

#  include "eevee_game_fsr3_shared.hh"
#endif

#include "eevee_game_fsr3_shared.hh"
#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(eevee_game_fsr3_common)
TYPEDEF_SOURCE("eevee_game_fsr3_shared.hh")
UNIFORM_BUF(0, FSR3UpscalerData, fsr3_buf)
GPU_SHADER_CREATE_END()

/* Render resolution. Dilates depth and motion, reprojects the depth to the previous frame,
 * writes the luma of the frame and the farthest depth of each 2x2 quad. */
GPU_SHADER_CREATE_INFO(eevee_game_fsr3_prepare_inputs)
DO_STATIC_COMPILATION()
ADDITIONAL_INFO(eevee_game_fsr3_common)
LOCAL_GROUP_SIZE(FSR3_GROUP_SIZE, FSR3_GROUP_SIZE)
DEFINE("FSR3UPSCALER_BIND_SRV_INPUT_MOTION_VECTORS")
DEFINE("FSR3UPSCALER_BIND_SRV_INPUT_DEPTH")
DEFINE("FSR3UPSCALER_BIND_SRV_INPUT_COLOR")
DEFINE("FSR3UPSCALER_BIND_UAV_DILATED_MOTION_VECTORS")
DEFINE("FSR3UPSCALER_BIND_UAV_DILATED_DEPTH")
DEFINE("FSR3UPSCALER_BIND_UAV_RECONSTRUCTED_PREV_NEAREST_DEPTH")
DEFINE("FSR3UPSCALER_BIND_UAV_FARTHEST_DEPTH")
DEFINE("FSR3UPSCALER_BIND_UAV_CURRENT_LUMA")
SAMPLER(0, sampler2D, r_input_motion_vectors)
SAMPLER(1, sampler2DDepth, r_input_depth)
SAMPLER(2, sampler2D, r_input_color_jittered)
IMAGE(0, SFLOAT_16_16, write, image2D, rw_dilated_motion_vectors)
IMAGE(1, SFLOAT_32, write, image2D, rw_dilated_depth)
IMAGE(2, UINT_32, read_write, uimage2DAtomic, rw_reconstructed_previous_nearest_depth)
IMAGE(3, SFLOAT_16, write, image2D, rw_farthest_depth)
IMAGE(4, SFLOAT_16, write, image2D, rw_current_luma)
COMPUTE_SOURCE("eevee_game_fsr3_prepare_inputs_comp.glsl")
GPU_SHADER_CREATE_END()

/* SPD over the luma and the farthest depth: frame info (auto exposure, average luma) and the
 * farthest depth at half resolution. */
GPU_SHADER_CREATE_INFO(eevee_game_fsr3_luma_pyramid)
DO_STATIC_COMPILATION()
ADDITIONAL_INFO(eevee_game_fsr3_common)
LOCAL_GROUP_SIZE(FSR3_SPD_GROUP_SIZE)
DEFINE("FSR3UPSCALER_BIND_SRV_CURRENT_LUMA")
DEFINE("FSR3UPSCALER_BIND_SRV_FARTHEST_DEPTH")
DEFINE("FSR3UPSCALER_BIND_UAV_SPD_GLOBAL_ATOMIC")
DEFINE("FSR3UPSCALER_BIND_UAV_FRAME_INFO")
DEFINE("FSR3UPSCALER_BIND_UAV_SPD_MIPS_LEVEL_0")
DEFINE("FSR3UPSCALER_BIND_UAV_FARTHEST_DEPTH_MIP1")
SAMPLER(0, sampler2D, r_current_luma)
SAMPLER(1, sampler2D, r_farthest_depth)
STORAGE_BUF(0, read_write, uint, rw_spd_global_atomic[])
STORAGE_BUF(1, read_write, float4, rw_frame_info[])
IMAGE(0, SFLOAT_16_16, write, image2D, rw_spd_mip0)
IMAGE(1, SFLOAT_16_16, write, image2D, rw_spd_mip1)
IMAGE(2, SFLOAT_16_16, write, image2D, rw_spd_mip2)
IMAGE(3, SFLOAT_16_16, write, image2D, rw_spd_mip3)
IMAGE(4, SFLOAT_16_16, write, image2D, rw_spd_mip4)
/* Read back by the last work-group. */
IMAGE(5, SFLOAT_16_16, read_write, image2D, rw_spd_mip5)
IMAGE(6, SFLOAT_16, write, image2D, rw_farthest_depth_mip1)
COMPUTE_SOURCE("eevee_game_fsr3_luma_pyramid_comp.glsl")
GPU_SHADER_CREATE_END()

/* SPD over the luma change between this frame and the reprojected previous one. */
GPU_SHADER_CREATE_INFO(eevee_game_fsr3_shading_change_pyramid)
DO_STATIC_COMPILATION()
ADDITIONAL_INFO(eevee_game_fsr3_common)
LOCAL_GROUP_SIZE(FSR3_SPD_GROUP_SIZE)
DEFINE("FSR3UPSCALER_BIND_SRV_CURRENT_LUMA")
DEFINE("FSR3UPSCALER_BIND_SRV_PREVIOUS_LUMA")
DEFINE("FSR3UPSCALER_BIND_SRV_DILATED_MOTION_VECTORS")
DEFINE("FSR3UPSCALER_BIND_UAV_SPD_GLOBAL_ATOMIC")
DEFINE("FSR3UPSCALER_BIND_UAV_SPD_MIPS_LEVEL_0")
SAMPLER(0, sampler2D, r_current_luma)
SAMPLER(1, sampler2D, r_previous_luma)
SAMPLER(2, sampler2D, r_dilated_motion_vectors)
STORAGE_BUF(0, read_write, uint, rw_spd_global_atomic[])
IMAGE(0, SFLOAT_16_16, write, image2D, rw_spd_mip0)
IMAGE(1, SFLOAT_16_16, write, image2D, rw_spd_mip1)
IMAGE(2, SFLOAT_16_16, write, image2D, rw_spd_mip2)
IMAGE(3, SFLOAT_16_16, write, image2D, rw_spd_mip3)
IMAGE(4, SFLOAT_16_16, write, image2D, rw_spd_mip4)
IMAGE(5, SFLOAT_16_16, read_write, image2D, rw_spd_mip5)
COMPUTE_SOURCE("eevee_game_fsr3_shading_change_pyramid_comp.glsl")
GPU_SHADER_CREATE_END()

/* Half render resolution: shading change from the pyramid. */
GPU_SHADER_CREATE_INFO(eevee_game_fsr3_shading_change)
DO_STATIC_COMPILATION()
ADDITIONAL_INFO(eevee_game_fsr3_common)
LOCAL_GROUP_SIZE(FSR3_GROUP_SIZE, FSR3_GROUP_SIZE)
DEFINE("FSR3UPSCALER_BIND_SRV_SPD_MIPS")
DEFINE("FSR3UPSCALER_BIND_UAV_SHADING_CHANGE")
/* Linear filtering between texels, nearest mip (textureLod with integer levels). */
SAMPLER(0, sampler2D, r_spd_mips)
IMAGE(0, UNORM_8, write, image2D, rw_shading_change)
COMPUTE_SOURCE("eevee_game_fsr3_shading_change_comp.glsl")
GPU_SHADER_CREATE_END()

/* Render resolution: disocclusion, reactive and shading change masks, accumulation, locks. */
GPU_SHADER_CREATE_INFO(eevee_game_fsr3_prepare_reactivity)
DO_STATIC_COMPILATION()
ADDITIONAL_INFO(eevee_game_fsr3_common)
LOCAL_GROUP_SIZE(FSR3_GROUP_SIZE, FSR3_GROUP_SIZE)
DEFINE("FSR3UPSCALER_BIND_SRV_RECONSTRUCTED_PREV_NEAREST_DEPTH")
DEFINE("FSR3UPSCALER_BIND_SRV_DILATED_MOTION_VECTORS")
DEFINE("FSR3UPSCALER_BIND_SRV_DILATED_DEPTH")
DEFINE("FSR3UPSCALER_BIND_SRV_REACTIVE_MASK")
DEFINE("FSR3UPSCALER_BIND_SRV_TRANSPARENCY_AND_COMPOSITION_MASK")
DEFINE("FSR3UPSCALER_BIND_SRV_ACCUMULATION")
DEFINE("FSR3UPSCALER_BIND_SRV_SHADING_CHANGE")
DEFINE("FSR3UPSCALER_BIND_SRV_CURRENT_LUMA")
DEFINE("FSR3UPSCALER_BIND_UAV_DILATED_REACTIVE_MASKS")
DEFINE("FSR3UPSCALER_BIND_UAV_NEW_LOCKS")
DEFINE("FSR3UPSCALER_BIND_UAV_ACCUMULATION")
SAMPLER(0, usampler2DAtomic, r_reconstructed_previous_nearest_depth)
SAMPLER(1, sampler2D, r_dilated_motion_vectors)
SAMPLER(2, sampler2D, r_dilated_depth)
SAMPLER(3, sampler2D, r_reactive_mask)
SAMPLER(4, sampler2D, r_transparency_and_composition_mask)
SAMPLER(5, sampler2D, r_accumulation)
SAMPLER(6, sampler2D, r_shading_change)
SAMPLER(7, sampler2D, r_current_luma)
IMAGE(0, UNORM_8_8_8_8, write, image2D, rw_dilated_reactive_masks)
IMAGE(1, UNORM_8, read_write, image2D, rw_new_locks)
IMAGE(2, UNORM_8, write, image2D, rw_accumulation)
COMPUTE_SOURCE("eevee_game_fsr3_prepare_reactivity_comp.glsl")
GPU_SHADER_CREATE_END()

/* Render resolution: luma instability from the luma history. */
GPU_SHADER_CREATE_INFO(eevee_game_fsr3_luma_instability)
DO_STATIC_COMPILATION()
ADDITIONAL_INFO(eevee_game_fsr3_common)
LOCAL_GROUP_SIZE(FSR3_GROUP_SIZE, FSR3_GROUP_SIZE)
DEFINE("FSR3UPSCALER_BIND_SRV_DILATED_REACTIVE_MASKS")
DEFINE("FSR3UPSCALER_BIND_SRV_DILATED_MOTION_VECTORS")
DEFINE("FSR3UPSCALER_BIND_SRV_FRAME_INFO")
DEFINE("FSR3UPSCALER_BIND_SRV_LUMA_HISTORY")
DEFINE("FSR3UPSCALER_BIND_SRV_FARTHEST_DEPTH_MIP1")
DEFINE("FSR3UPSCALER_BIND_SRV_CURRENT_LUMA")
DEFINE("FSR3UPSCALER_BIND_UAV_LUMA_HISTORY")
DEFINE("FSR3UPSCALER_BIND_UAV_LUMA_INSTABILITY")
SAMPLER(0, sampler2D, r_dilated_reactive_masks)
SAMPLER(1, sampler2D, r_dilated_motion_vectors)
SAMPLER(2, sampler2D, r_luma_history)
SAMPLER(3, sampler2D, r_farthest_depth_mip1)
SAMPLER(4, sampler2D, r_current_luma)
STORAGE_BUF(0, read, float4, r_frame_info[])
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, rw_luma_history)
IMAGE(1, SFLOAT_16, write, image2D, rw_luma_instability)
COMPUTE_SOURCE("eevee_game_fsr3_luma_instability_comp.glsl")
GPU_SHADER_CREATE_END()

/* Output resolution: reprojection, upsampling and accumulation. */
GPU_SHADER_CREATE_INFO(eevee_game_fsr3_accumulate_base)
ADDITIONAL_INFO(eevee_game_fsr3_common)
LOCAL_GROUP_SIZE(FSR3_GROUP_SIZE, FSR3_GROUP_SIZE)
DEFINE("FSR3UPSCALER_BIND_SRV_DILATED_REACTIVE_MASKS")
DEFINE("FSR3UPSCALER_BIND_SRV_DILATED_MOTION_VECTORS")
DEFINE("FSR3UPSCALER_BIND_SRV_INTERNAL_UPSCALED")
DEFINE("FSR3UPSCALER_BIND_SRV_FARTHEST_DEPTH_MIP1")
DEFINE("FSR3UPSCALER_BIND_SRV_CURRENT_LUMA")
DEFINE("FSR3UPSCALER_BIND_SRV_LUMA_INSTABILITY")
DEFINE("FSR3UPSCALER_BIND_SRV_INPUT_COLOR")
DEFINE("FSR3UPSCALER_BIND_UAV_INTERNAL_UPSCALED")
DEFINE("FSR3UPSCALER_BIND_UAV_UPSCALED_OUTPUT")
DEFINE("FSR3UPSCALER_BIND_UAV_NEW_LOCKS")
SAMPLER(0, sampler2D, r_dilated_reactive_masks)
SAMPLER(1, sampler2D, r_dilated_motion_vectors)
SAMPLER(2, sampler2D, r_internal_upscaled_color)
SAMPLER(3, sampler2D, r_farthest_depth_mip1)
SAMPLER(4, sampler2D, r_current_luma)
SAMPLER(5, sampler2D, r_luma_instability)
SAMPLER(6, sampler2D, r_input_color_jittered)
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, rw_internal_upscaled_color)
IMAGE(1, SFLOAT_16_16_16_16, write, image2D, rw_upscaled_output)
IMAGE(2, UNORM_8, read_write, image2D, rw_new_locks)
COMPUTE_SOURCE("eevee_game_fsr3_accumulate_comp.glsl")
GPU_SHADER_CREATE_END()

/* Writes the output: no sharpening. */
GPU_SHADER_CREATE_INFO(eevee_game_fsr3_accumulate)
DO_STATIC_COMPILATION()
DEFINE_VALUE("FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING", "0")
ADDITIONAL_INFO(eevee_game_fsr3_accumulate_base)
GPU_SHADER_CREATE_END()

/* Leaves the output to RCAS. */
GPU_SHADER_CREATE_INFO(eevee_game_fsr3_accumulate_sharpen)
DO_STATIC_COMPILATION()
DEFINE_VALUE("FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING", "1")
ADDITIONAL_INFO(eevee_game_fsr3_accumulate_base)
GPU_SHADER_CREATE_END()

/* Output resolution: robust contrast adaptive sharpening of the accumulated color. */
GPU_SHADER_CREATE_INFO(eevee_game_fsr3_rcas)
DO_STATIC_COMPILATION()
ADDITIONAL_INFO(eevee_game_fsr3_common)
LOCAL_GROUP_SIZE(FSR3_RCAS_GROUP_SIZE)
DEFINE("FSR3UPSCALER_BIND_SRV_RCAS_INPUT")
DEFINE("FSR3UPSCALER_BIND_UAV_UPSCALED_OUTPUT")
SAMPLER(0, sampler2D, r_rcas_input)
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, rw_upscaled_output)
COMPUTE_SOURCE("eevee_game_fsr3_rcas_comp.glsl")
GPU_SHADER_CREATE_END()

/* Render resolution: reactive mask from the color before and after the transparent surfaces
 * (FidelityFX's ffxFsr3UpscalerContextGenerateReactiveMask). */
GPU_SHADER_CREATE_INFO(eevee_game_fsr3_autogen_reactive)
DO_STATIC_COMPILATION()
ADDITIONAL_INFO(eevee_game_fsr3_common)
LOCAL_GROUP_SIZE(FSR3_GROUP_SIZE, FSR3_GROUP_SIZE)
DEFINE("FSR3UPSCALER_BIND_SRV_INPUT_OPAQUE_ONLY")
DEFINE("FSR3UPSCALER_BIND_SRV_INPUT_COLOR")
DEFINE("FSR3UPSCALER_BIND_UAV_AUTOREACTIVE")
SAMPLER(0, sampler2D, r_input_opaque_only)
SAMPLER(1, sampler2D, r_input_color_jittered)
IMAGE(0, UNORM_8, write, image2D, rw_output_autoreactive)
COMPUTE_SOURCE("eevee_game_fsr3_autogen_reactive_comp.glsl")
GPU_SHADER_CREATE_END()
