/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Structures shared between C++ and GLSL.
 *
 * Anything a shader reads from a buffer has to be declared here and nowhere
 * else, so both sides compile the same layout. Only add data that actually
 * crosses to the GPU; CPU-only bookkeeping stays in the module headers.
 */

#pragma once

#include "GPU_shader_shared_utils.hh"

#ifndef GPU_SHADER
namespace blender::eevee_game {
#endif

/* LightData::type. Mirrors `enum class LightType` (eevee_game_defines.hh),
 * which static-asserts against these so the two cannot drift apart. */
#define LIGHT_TYPE_SUN 0u
#define LIGHT_TYPE_POINT 1u
#define LIGHT_TYPE_SPOT 2u
#define LIGHT_TYPE_AREA_RECT 3u
#define LIGHT_TYPE_AREA_ELLIPSE 4u

/* One light, 64 bytes: exactly one cache line per light in the shading loop.
 * Each float3 is followed by a scalar so the std430 layout has no implicit
 * padding and matches the C++ struct byte for byte. */
struct [[host_shared]] LightData {
  packed_float3 position;
  uint type;

  packed_float3 color;
  float energy;

  /** Spot and sun lights. */
  packed_float3 direction;
  /** Emitter size: soft shadow radius, or panel size for area lights. */
  float radius;

  /** Influence distance; the light contributes nothing beyond it. */
  float attenuation;
  float spot_angle;
  /** Shadow atlas slot, -1 = no shadow. */
  int shadow_index;
  /** LIGHT_FLAG_* bits. */
  uint flags;
};

#ifndef GPU_SHADER
}  // namespace blender::eevee_game
#endif
