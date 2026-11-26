/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Data shared by TemporalModule (HWTAA) and its shaders.
 */

#pragma once

#include "GPU_shader_shared_utils.hh"

/* Compute groups of the store, reactive and resolve passes. */
#define TEMPORAL_GROUP_SIZE 8
/* Render texels of the resolve tile per axis: ceil(7 * render scale) + 3, at most 10. */
#define TEMPORAL_TILE_MAX 10
/* Gaussian of the reconstruction, exp(-k d^2), d in output pixels. */
#define TEMPORAL_RECONSTRUCTION_SHARPNESS 4.0f

/* Motion table entry, per draw manager resource: stable object key (16 bits), motion chain slot
 * (13 bits, 0: none) and flags. */
#define TEMPORAL_KEY_MASK 0xFFFFu
#define TEMPORAL_SLOT_SHIFT 16
#define TEMPORAL_SLOT_MASK 0x1FFFu
#define TEMPORAL_MOVED_BIT 0x20000000u
#define TEMPORAL_VOLATILE_BIT 0x40000000u
#define TEMPORAL_DEFORM_BIT 0x80000000u

/* Ring info word: key, then the half float inverse depth whose sign flags a surface that is not
 * static and whose lowest mantissa bit flags a sample under transparent surfaces. */
#define TEMPORAL_INFO_UNSTABLE_BIT 0x80000000u
#define TEMPORAL_INFO_TRANSPARENT_BIT 0x00010000u

/* Reactive mask: a texel above the threshold is under transparent surfaces. */
#define TEMPORAL_REACTIVE_THRESHOLD 0.1f
#define TEMPORAL_REACTIVE_SCALE 4.0f

/* Window: frames at most, float4 rows per frame (age) and their layout. */
#define TEMPORAL_WINDOW_MAX 16
#define TEMPORAL_AGE_STRIDE 10
#define TEMPORAL_AGE_REPROJECT 0   /* 4 columns: texel and inverse depth to window units. */
#define TEMPORAL_AGE_VIEWPROJ 4    /* 4 columns: unjittered view projection of that frame. */
#define TEMPORAL_AGE_DEPTH_PARAMS 8
#define TEMPORAL_AGE_JITTER 9      /* jitter (render pixels), ring layer. */

#ifndef GPU_SHADER
namespace blender::eevee_game {
#endif

/** Per frame of the window, `age` frames ago: see TEMPORAL_AGE_*. */
struct [[host_shared]] TemporalWindowData {
  float4 rows[TEMPORAL_WINDOW_MAX * TEMPORAL_AGE_STRIDE];
};

#ifndef GPU_SHADER
}  // namespace blender::eevee_game
#endif
