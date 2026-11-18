/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Diffuse world lighting: L1 spherical harmonics of the captured radiance.
 *
 * With c0 = integral of L and c1 = integral of L w over the sphere, the
 * Lambert convolution of the L1 projection gives
 *   E(n) / pi = c0 / (4 pi) + dot(c1, n) / (2 pi)
 * (Y00^2 = 1 / (4 pi), Y1^2 = 3 / (4 pi), band factors pi and 2 pi / 3).
 * Stored per channel as float4(A, B) so the lookup is max(dot(sh, (1, n)), 0):
 * a constant sky of radiance L gives A = L, B = 0.
 *
 * One work group, run only when the world changes: 6 x 16 x 16 texels of a
 * low mip, each weighted by its solid angle. The weights are normalized by
 * their own sum, which is 4 pi up to the discretization.
 */

#include "infos/eevee_game_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_game_world_sh)

#include "eevee_game_world_lib.glsl"

shared float4 sh_r[64];
shared float4 sh_g[64];
shared float4 sh_b[64];
shared float sh_w[64];

void main()
{
  int local_index = int(gl_LocalInvocationIndex);
  const int face_len = WORLD_SH_RES * WORLD_SH_RES;

  float4 acc_r = float4(0.0f);
  float4 acc_g = float4(0.0f);
  float4 acc_b = float4(0.0f);
  float acc_w = 0.0f;
  for (int i = local_index; i < 6 * face_len; i += 64) {
    int face = i / face_len;
    int rem = i - face * face_len;
    int2 texel = int2(rem % WORLD_SH_RES, rem / WORLD_SH_RES);
    float2 st = (float2(texel) + 0.5f) * (2.0f / float(WORLD_SH_RES)) - 1.0f;
    float3 dir = cube_face_direction(face, st);
    /* Solid angle of a texel at st on the unit cube, up to the constant
     * texel area: 1 / (1 + s^2 + t^2)^(3/2). */
    float len_sqr = dot(dir, dir);
    float w = inversesqrt(len_sqr) / len_sqr;
    float3 n = dir * inversesqrt(len_sqr);
    float3 L = textureLod(radiance_tx, n, sh_lod).rgb * w;
    acc_r += float4(1.0f, n) * L.r;
    acc_g += float4(1.0f, n) * L.g;
    acc_b += float4(1.0f, n) * L.b;
    acc_w += w;
  }
  sh_r[local_index] = acc_r;
  sh_g[local_index] = acc_g;
  sh_b[local_index] = acc_b;
  sh_w[local_index] = acc_w;
  barrier();

  if (local_index != 0) {
    return;
  }
  float4 r = float4(0.0f), g = float4(0.0f), b = float4(0.0f);
  float w_sum = 0.0f;
  for (int i = 0; i < 64; i++) {
    r += sh_r[i];
    g += sh_g[i];
    b += sh_b[i];
    w_sum += sh_w[i];
  }
  /* A = c0 / (4 pi), B = c1 / (2 pi), with the integrals 4 pi * sum / w_sum. */
  float4 scale = float4(1.0f, 2.0f, 2.0f, 2.0f) / w_sum;
  out_sh_buf[0] = r * scale;
  out_sh_buf[1] = g * scale;
  out_sh_buf[2] = b * scale;
}
