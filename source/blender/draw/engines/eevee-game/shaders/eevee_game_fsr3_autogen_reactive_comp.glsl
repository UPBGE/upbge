/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * FSR 3.1 upscaler: reactive mask from the opaque and the final color of the frame.
 * Entry point of FidelityFX's ffx_fsr3upscaler_autogen_reactive_pass.glsl (SDK v1.1.4).
 */

#include "infos/eevee_game_fsr3_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_game_fsr3_autogen_reactive)

#include "eevee_game_fsr3_callbacks_lib.glsl"
#include "ffx_core_lib.glsl"
#include "ffx_fsr3upscaler_common_lib.glsl"

void main()
{
  const int2 iPxPos = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(iPxPos, RenderSize()))) {
    return;
  }

  float3 ColorPreAlpha = LoadOpaqueOnly(iPxPos).rgb;
  float3 ColorPostAlpha = LoadInputColor(iPxPos).rgb;

  if ((GenReactiveFlags() & FSR3_AUTOREACTIVE_APPLY_TONEMAP) != 0u) {
    ColorPreAlpha = Tonemap(ColorPreAlpha);
    ColorPostAlpha = Tonemap(ColorPostAlpha);
  }

  if ((GenReactiveFlags() & FSR3_AUTOREACTIVE_APPLY_INVERSETONEMAP) != 0u) {
    ColorPreAlpha = InverseTonemap(ColorPreAlpha);
    ColorPostAlpha = InverseTonemap(ColorPostAlpha);
  }

  float out_reactive_value = 0.0f;
  float3 delta = abs(ColorPostAlpha - ColorPreAlpha);

  out_reactive_value = ((GenReactiveFlags() & FSR3_AUTOREACTIVE_USE_COMPONENTS_MAX) != 0u) ?
                           max(delta.x, max(delta.y, delta.z)) :
                           length(delta);
  out_reactive_value *= GenReactiveScale();

  out_reactive_value = ((GenReactiveFlags() & FSR3_AUTOREACTIVE_APPLY_THRESHOLD) != 0u) ?
                           ((out_reactive_value < GenReactiveThreshold()) ?
                                0.0f :
                                GenReactiveBinaryValue()) :
                           out_reactive_value;

  StoreAutoReactive(iPxPos, out_reactive_value);
}
