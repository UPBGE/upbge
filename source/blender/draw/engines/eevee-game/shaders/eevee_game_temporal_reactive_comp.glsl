/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Reactive mask of the transparent surfaces, render resolution, in two dispatches around the
 * forward transparent pass:
 * - capture (before): the opaque color, tone mapped c / (1 + max(c)) in 8 bits per channel;
 * - mask (after): the largest tone mapped channel change, times TEMPORAL_REACTIVE_SCALE. It
 *   measures what the transparent surfaces did to the texel, so alpha blended and additive
 *   surfaces count alike, and a transparent surface that changes nothing (the empty part of a
 *   sprite) does not.
 * 8 bit steps of the tone mapped color are 0.016 of the mask: far below the threshold
 * (TEMPORAL_REACTIVE_THRESHOLD). Per texel: 12 bytes for the capture, 13 for the mask.
 */

#include "infos/eevee_game_temporal_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_game_temporal_reactive)

#include "eevee_game_temporal_lib.glsl"

void main()
{
  const int2 texel = int2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(texel, render_size))) {
    return;
  }
  const float3 color = sanitize_color(texelFetch(color_tx, texel, 0).rgb);
  const float3 mapped = color * (1.0 / (1.0 + max3(color)));
  if (capture) {
    imageStore(opaque_img, texel, float4(mapped, 0.0));
    return;
  }
  const float3 opaque = imageLoad(opaque_img, texel).rgb;
  const float reactive = min(max3(abs(mapped - opaque)) * TEMPORAL_REACTIVE_SCALE, 1.0);
  imageStore(reactive_img, texel, float4(reactive));
}
