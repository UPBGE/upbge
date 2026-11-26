/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * HWTAA: temporal anti-aliasing and upscaling over a ring of past render samples
 * (eevee_game_temporal.hh).
 */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#endif

#include "gpu_shader_create_info.hh"

#include "eevee_game_temporal_shared.hh"

/* Reactive mask of the transparent surfaces: `capture` stores the opaque color before them, the
 * second dispatch the mask after them. */
GPU_SHADER_CREATE_INFO(eevee_game_temporal_reactive)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(8, 8) /* TEMPORAL_GROUP_SIZE */
TYPEDEF_SOURCE("eevee_game_temporal_shared.hh")
SAMPLER(0, sampler2D, color_tx)
IMAGE(0, UNORM_8_8_8_8, read_write, image2D, opaque_img)
IMAGE(1, UNORM_8, write, image2D, reactive_img)
PUSH_CONSTANT(int2, render_size)
PUSH_CONSTANT(bool, capture)
COMPUTE_SOURCE("eevee_game_temporal_reactive_comp.glsl")
GPU_SHADER_CREATE_END()

/* This frame's samples into one layer of the ring. */
GPU_SHADER_CREATE_INFO(eevee_game_temporal_store)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(8, 8) /* TEMPORAL_GROUP_SIZE */
TYPEDEF_SOURCE("eevee_game_temporal_shared.hh")
SAMPLER(0, sampler2D, color_tx)
SAMPLER(1, sampler2D, depth_tx)
SAMPLER(2, usampler2D, object_id_tx)
SAMPLER(3, sampler2D, reactive_tx)
STORAGE_BUF(0, read, uint, motion_buf[])
IMAGE(0, UINT_32_32, write, uimage2DArray, ring_img)
PUSH_CONSTANT(float4, depth_params)
PUSH_CONSTANT(int2, render_size)
PUSH_CONSTANT(int, motion_len)
PUSH_CONSTANT(int, ring_layer)
COMPUTE_SOURCE("eevee_game_temporal_store_comp.glsl")
GPU_SHADER_CREATE_END()

/* Output pixels: history for still pixels, sample window for moving ones. */
GPU_SHADER_CREATE_INFO(eevee_game_temporal_resolve)
DO_STATIC_COMPILATION()
LOCAL_GROUP_SIZE(8, 8) /* TEMPORAL_GROUP_SIZE */
TYPEDEF_SOURCE("eevee_game_temporal_shared.hh")
SAMPLER(0, usampler2DArray, ring_tx)
SAMPLER(1, sampler2D, depth_tx)
SAMPLER(2, usampler2D, object_id_tx)
SAMPLER(3, sampler2D, vector_tx)
SAMPLER(4, sampler2D, history_tx)
SAMPLER(5, sampler2D, reactive_tx)
SAMPLER(6, sampler2D, lock_tx)
SAMPLER(7, sampler2D, color_tx)
STORAGE_BUF(0, read, uint, motion_buf[])
STORAGE_BUF(1, read, float4, chain_buf[])
UNIFORM_BUF(0, TemporalWindowData, window_data)
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, history_out_img)
IMAGE(1, UNORM_8_8, write, image2D, lock_out_img)
/* The output for the present pass: the history's alpha is its confidence, not a coverage. */
IMAGE(2, SFLOAT_16_16_16_16, write, image2D, out_color_img)
PUSH_CONSTANT(float4x4, curr_viewprojinv)
PUSH_CONSTANT(float4x4, prev_reproject)
PUSH_CONSTANT(float4, curr_depth_params)
PUSH_CONSTANT(float2, render_scale)
PUSH_CONSTANT(float2, jitter_px)
PUSH_CONSTANT(int2, render_size)
PUSH_CONSTANT(int2, display_size)
PUSH_CONSTANT(int, tile_dim)
PUSH_CONSTANT(int, motion_len)
PUSH_CONSTANT(int, ring_layer)
PUSH_CONSTANT(int, window_frames)
PUSH_CONSTANT(int, chain_stride)
PUSH_CONSTANT(float, max_confidence)
PUSH_CONSTANT(float, lock_decay)
PUSH_CONSTANT(bool, history_valid)
PUSH_CONSTANT(bool, window_volatile)
PUSH_CONSTANT(bool, frequency_split)
SPECIALIZATION_CONSTANT(bool, window_ortho, false)
COMPUTE_SOURCE("eevee_game_temporal_resolve_comp.glsl")
GPU_SHADER_CREATE_END()
