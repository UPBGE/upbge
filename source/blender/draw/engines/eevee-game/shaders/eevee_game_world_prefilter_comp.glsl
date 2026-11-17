/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Specular world lighting: GGX pre-filtered radiance, one mip per roughness.
 *
 * Split sum with V = N = R: each texel integrates the radiance under the GGX
 * lobe of its mip's roughness around its own direction. EEVEE's closure
 * color already carries the BRDF integral, so only the radiance half is
 * stored. Filtered importance sampling (Krivanek and Colbert): every sample
 * reads the source mip whose texel matches the solid angle the sample
 * stands for, so 32 samples are enough without fireflies.
 *
 * Run only when the world changes.
 */

#include "infos/eevee_game_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_game_world_prefilter)

#include "eevee_game_world_lib.glsl"
#include "gpu_shader_math_constants_lib.glsl"

#define SAMPLE_COUNT 32

void main()
{
  int3 texel = int3(gl_GlobalInvocationID.xyz);
  int2 size = imageSize(out_img).xy;
  if (any(greaterThanEqual(texel.xy, size))) {
    return;
  }
  float2 st = (float2(texel.xy) + 0.5f) / float2(size) * 2.0f - 1.0f;
  float3 N = normalize(cube_face_direction(texel.z, st));

  if (roughness == 0.0f) {
    /* Mirror: the source itself. */
    imageStore(out_img, texel, float4(textureLod(radiance_tx, N, 0.0f).rgb, 1.0f));
    return;
  }

  /* Tangent frame around N (Duff et al. 2017). */
  float sign_z = (N.z >= 0.0f) ? 1.0f : -1.0f;
  float a_ = -1.0f / (sign_z + N.z);
  float b_ = N.x * N.y * a_;
  float3 T = float3(1.0f + sign_z * N.x * N.x * a_, sign_z * b_, -sign_z * N.x);
  float3 B = float3(b_, sign_z + N.y * N.y * a_, -N.y);

  float a = roughness * roughness;
  float a2 = a * a;
  float3 sum = float3(0.0f);
  float weight = 0.0f;
  for (int i = 0; i < SAMPLE_COUNT; i++) {
    /* Hammersley point set. */
    float u = (float(i) + 0.5f) / float(SAMPLE_COUNT);
    float v = float(bitfieldReverse(uint(i))) * 2.3283064e-10f;
    /* GGX distributed half vector. */
    float cos_theta = sqrt((1.0f - v) / (1.0f + (a2 - 1.0f) * v));
    float sin_theta = sqrt(max(1.0f - cos_theta * cos_theta, 0.0f));
    float phi = 2.0f * M_PI * u;
    float3 H = T * (sin_theta * cos(phi)) + B * (sin_theta * sin(phi)) + N * cos_theta;
    float3 L = 2.0f * cos_theta * H - N;
    float NL = dot(N, L);
    if (NL <= 0.0f) {
      continue;
    }
    /* pdf(L) = D(H) NH / (4 VH) = D / 4 with V = N. */
    float d = cos_theta * cos_theta * (a2 - 1.0f) + 1.0f;
    float pdf = a2 / (4.0f * M_PI * d * d);
    float sample_solid_angle = 1.0f / (float(SAMPLE_COUNT) * pdf);
    /* One mip up from the matching footprint: smoother at no cost. */
    float lod = max(0.5f * log2(sample_solid_angle / texel_solid_angle) + 1.0f, 0.0f);
    sum += textureLod(radiance_tx, L, lod).rgb * NL;
    weight += NL;
  }
  imageStore(out_img, texel, float4(sum / max(weight, 1e-6f), 1.0f));
}
