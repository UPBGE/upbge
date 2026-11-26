/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Temporal store: writes this frame's render samples into one layer of the sample ring, once,
 * at render resolution, so the resolve can read any of the last frames at the same cost.
 *
 * Per render texel:
 * - color, sanitized, and the median of the tone mapped luma of the 3x3 around it, in 8 bits
 *   per channel (ring_color_encode). The window reads the color of past frames (the current
 *   frame comes from the full precision input). The resolve compares the median across frames
 *   to drop the frames whose shading changed (moving shadows) and to kill locks;
 * - stable object key and inverse depth, the sign flagging surfaces that are not static. The
 *   resolve validates against them without the resource id tables of past frames;
 * - whether transparent surfaces cover it (reactive mask, TEMPORAL_INFO_TRANSPARENT_BIT).
 */

#include "infos/eevee_game_temporal_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_game_temporal_store)

#include "eevee_game_temporal_lib.glsl"

#define STORE_TILE_SIZE (TEMPORAL_GROUP_SIZE + 2)
#define STORE_TILE_LEN (STORE_TILE_SIZE * STORE_TILE_SIZE)
#define STORE_GROUP_LEN (TEMPORAL_GROUP_SIZE * TEMPORAL_GROUP_SIZE)

/* Tone mapped luma (median) and linear color (the center's ring color) of the 10x10 tile. */
shared float tile_luma[100]; /* STORE_TILE_LEN: literal, shared arrays are declared before the defines. */
shared float3 tile_color[100];

/* Texel i of the tile: (i mod 10, i / 10), the quotient in float ((i + 0.5) / 10 stays 0.05 away
 * from an integer). */
int2 tile_texel(int2 tile_origin, int i)
{
  const float row = floor((float(i) + 0.5) * (1.0 / float(STORE_TILE_SIZE)));
  const int2 local = int2(float(i) - row * float(STORE_TILE_SIZE), row);
  return clamp(tile_origin + local, int2(0), render_size - 1);
}

void tile_store(int i, float3 color)
{
  color = sanitize_color(color);
  tile_color[i] = color;
  tile_luma[i] = color_to_space(color, 1.0).x;
}

void main()
{
  /* Every load first, so their latencies overlap: the 100 tile texels in two rounds (the second
   * one for 36 threads; the others load a valid texel and drop it), this texel's depth and id. */
  const int2 tile_origin = int2(gl_WorkGroupID.xy) * TEMPORAL_GROUP_SIZE - 1;
  const int i_0 = int(gl_LocalInvocationIndex);
  const int i_1 = min(i_0 + STORE_GROUP_LEN, STORE_TILE_LEN - 1);
  const float3 color_0 = texelFetch(color_tx, tile_texel(tile_origin, i_0), 0).rgb;
  const float3 color_1 = texelFetch(color_tx, tile_texel(tile_origin, i_1), 0).rgb;
  const int2 texel = int2(gl_GlobalInvocationID.xy);
  const int2 texel_clamped = min(texel, render_size - 1);
  const float depth = texelFetch(depth_tx, texel_clamped, 0).r;
  const uint resource_id = texelFetch(object_id_tx, texel_clamped, 0).r;
  const uint entry = motion_buf[min(resource_id, uint(motion_len - 1))];
  /* By coordinates: without a mask the 1x1 dummy is bound. */
  const float reactive =
      textureLod(reactive_tx, (float2(texel_clamped) + 0.5) / float2(render_size), 0.0).r;

  tile_store(i_0, color_0);
  if (i_0 + STORE_GROUP_LEN < STORE_TILE_LEN) {
    tile_store(i_1, color_1);
  }
  barrier();

  if (any(greaterThanEqual(texel, render_size))) {
    return;
  }

  const int2 local = int2(gl_LocalInvocationID.xy);
  const int center = (local.y + 1) * STORE_TILE_SIZE + local.x + 1;
  float luma[9];
  for (int i = 0; i < 9; i++) {
    luma[i] = tile_luma[center + (i / 3 - 1) * STORE_TILE_SIZE + i % 3 - 1];
  }

  const bool unstable = (entry & (TEMPORAL_DEFORM_BIT |
                                  (TEMPORAL_SLOT_MASK << TEMPORAL_SLOT_SHIFT))) != 0u;
  uint info = (depth != 0.0) ?
                  info_pack(entry, inverse_depth_buffer(depth, depth_params), unstable) :
                  0u;
  info |= (reactive > TEMPORAL_REACTIVE_THRESHOLD) ? TEMPORAL_INFO_TRANSPARENT_BIT : 0u;
  imageStore(
      ring_img, int3(texel, ring_layer), ring_texel_pack(info, tile_color[center], median9(luma)));
}
