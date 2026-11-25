/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Data shared by UpscaleModule and the FSR 3.1 upscaler shaders (FidelityFX SDK v1.1.4, ported
 * to Blender's shader system in `shaders/fsr3/`).
 */

#pragma once

#include "GPU_shader_shared_utils.hh"

#include "eevee_game_fsr3_defines.hh"

/* Permutation, fixed for EEVEE: scene linear color, reversed depth (1 at the near plane),
 * render resolution motion vectors without jitter (VelocityModule). */
#define FSR3_OPTION_HDR_COLOR_INPUT 1
#define FSR3_OPTION_INVERTED_DEPTH 1

/* FidelityFX's FFX_FSR3UPSCALER_AUTOREACTIVEFLAGS_* (ffx_fsr3upscaler.h). */
#define FSR3_AUTOREACTIVE_APPLY_TONEMAP 1u
#define FSR3_AUTOREACTIVE_APPLY_INVERSETONEMAP 2u
#define FSR3_AUTOREACTIVE_APPLY_THRESHOLD 4u
#define FSR3_AUTOREACTIVE_USE_COMPONENTS_MAX 8u

#ifndef GPU_SHADER
namespace blender::eevee_game {
#endif

/**
 * One uniform buffer for every pass: the constant buffers of FidelityFX's passes, in their
 * order and std140 layout (cbFSR3Upscaler, cbSPD, cbRCAS, cbGenerateReactive), and the
 * exposure FidelityFX reads from a 1x1 texture.
 */
struct [[host_shared]] FSR3UpscalerData {
  /* cbFSR3Upscaler. */
  int2 render_size;
  int2 previous_frame_render_size;
  int2 upscale_size;
  int2 previous_frame_upscale_size;
  int2 max_render_size;
  int2 max_upscale_size;
  float4 device_to_view_depth;
  float2 jitter;
  float2 previous_frame_jitter;
  float2 motion_vector_scale;
  float2 downscale_factor;
  float2 motion_vector_jitter_cancellation;
  float tan_half_fov;
  float jitter_sequence_length;
  float delta_time;
  float delta_pre_exposure;
  float view_space_to_meters_factor;
  float frame_index;
  float velocity_factor;
  float reactiveness_scale;
  float shading_change_scale;
  float accumulation_added_per_frame;
  float min_disocclusion_accumulation;
  /* r_input_exposure: color scale into the range the heuristics expect. 0 means 1. */
  float exposure;
  float _pad0;
  float _pad1;
  /* cbSPD, shared by the luma and shading change pyramids. */
  uint spd_mips;
  uint spd_num_work_groups;
  uint2 spd_work_group_offset;
  uint2 spd_render_size;
  uint2 _pad2;
  /* cbRCAS. */
  uint4 rcas_config;
  /* cbGenerateReactive. */
  float gen_reactive_scale;
  float gen_reactive_threshold;
  float gen_reactive_binary_value;
  uint gen_reactive_flags;
};
BLI_STATIC_ASSERT_ALIGN(FSR3UpscalerData, 16)

#ifndef GPU_SHADER
}  // namespace blender::eevee_game
#endif
