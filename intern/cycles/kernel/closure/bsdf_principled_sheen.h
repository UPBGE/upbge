/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#pragma once

/* DISNEY PRINCIPLED SHEEN BRDF
 *
 * Shading model by Brent Burley (Disney): "Physically Based Shading at Disney" (2012)
 */

#include "kernel/closure/bsdf_util.h"

CCL_NAMESPACE_BEGIN

typedef struct PrincipledSheenBsdf {
  SHADER_CLOSURE_BASE;
  float avg_value;
} PrincipledSheenBsdf;

static_assert(sizeof(ShaderClosure) >= sizeof(PrincipledSheenBsdf),
              "PrincipledSheenBsdf is too large!");

ccl_device_inline float calculate_avg_principled_sheen_brdf(float3 N, float3 I)
{
  /* To compute the average, we set the half-vector to the normal, resulting in
   * NdotI = NdotL = NdotV = LdotH */
  float NdotI = dot(N, I);
  if (NdotI < 0.0f) {
    return 0.0f;
  }

  return schlick_fresnel(NdotI) * NdotI;
}

ccl_device Spectrum
calculate_principled_sheen_brdf(float3 N, float3 V, float3 L, float3 H, ccl_private float *pdf)
{
  float NdotL = dot(N, L);
  float NdotV = dot(N, V);

  if (NdotL < 0 || NdotV < 0) {
    *pdf = 0.0f;
    return zero_spectrum();
  }

  float LdotH = dot(L, H);

  float value = schlick_fresnel(LdotH) * NdotL;

  return make_spectrum(value);
}

ccl_device int bsdf_principled_sheen_setup(ccl_private const ShaderData *sd,
                                           ccl_private PrincipledSheenBsdf *bsdf)
{
  bsdf->type = CLOSURE_BSDF_PRINCIPLED_SHEEN_ID;
  bsdf->avg_value = calculate_avg_principled_sheen_brdf(bsdf->N, sd->I);
  bsdf->sample_weight *= bsdf->avg_value;
  return SD_BSDF | SD_BSDF_HAS_EVAL;
}

ccl_device Spectrum bsdf_principled_sheen_eval_reflect(ccl_private const ShaderClosure *sc,
                                                       const float3 I,
                                                       const float3 omega_in,
                                                       ccl_private float *pdf)
{
  ccl_private const PrincipledSheenBsdf *bsdf = (ccl_private const PrincipledSheenBsdf *)sc;

  float3 N = bsdf->N;
  float3 V = I;         // outgoing
  float3 L = omega_in;  // incoming
  float3 H = normalize(L + V);

  if (dot(N, omega_in) > 0.0f) {
    *pdf = fmaxf(dot(N, omega_in), 0.0f) * M_1_PI_F;
    return calculate_principled_sheen_brdf(N, V, L, H, pdf);
  }
  else {
    *pdf = 0.0f;
    return zero_spectrum();
  }
}

ccl_device Spectrum bsdf_principled_sheen_eval_transmit(ccl_private const ShaderClosure *sc,
                                                        const float3 I,
                                                        const float3 omega_in,
                                                        ccl_private float *pdf)
{
  *pdf = 0.0f;
  return zero_spectrum();
}

ccl_device int bsdf_principled_sheen_sample(ccl_private const ShaderClosure *sc,
                                            float3 Ng,
                                            float3 I,
                                            float randu,
                                            float randv,
                                            ccl_private Spectrum *eval,
                                            ccl_private float3 *omega_in,
                                            ccl_private float *pdf)
{
  ccl_private const PrincipledSheenBsdf *bsdf = (ccl_private const PrincipledSheenBsdf *)sc;

  float3 N = bsdf->N;

  sample_cos_hemisphere(N, randu, randv, omega_in, pdf);

  if (dot(Ng, *omega_in) > 0) {
    float3 H = normalize(I + *omega_in);

    *eval = calculate_principled_sheen_brdf(N, I, *omega_in, H, pdf);
  }
  else {
    *eval = zero_spectrum();
    *pdf = 0.0f;
  }
  return LABEL_REFLECT | LABEL_DIFFUSE;
}

CCL_NAMESPACE_END
