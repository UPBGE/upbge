/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#pragma once

/* DISNEY PRINCIPLED DIFFUSE BRDF
 *
 * Shading model by Brent Burley (Disney): "Physically Based Shading at Disney" (2012)
 *
 * "Extending the Disney BRDF to a BSDF with Integrated Subsurface Scattering" (2015)
 * For the separation of retro-reflection, "2.3 Dielectric BRDF with integrated
 * subsurface scattering"
 */

#include "kernel/closure/bsdf_util.h"

#include "kernel/sample/mapping.h"

CCL_NAMESPACE_BEGIN

enum PrincipledDiffuseBsdfComponents {
  PRINCIPLED_DIFFUSE_FULL = 1,
  PRINCIPLED_DIFFUSE_LAMBERT = 2,
  PRINCIPLED_DIFFUSE_LAMBERT_EXIT = 4,
  PRINCIPLED_DIFFUSE_RETRO_REFLECTION = 8,
};

typedef struct PrincipledDiffuseBsdf {
  SHADER_CLOSURE_BASE;

  float roughness;
  int components;
} PrincipledDiffuseBsdf;

static_assert(sizeof(ShaderClosure) >= sizeof(PrincipledDiffuseBsdf),
              "PrincipledDiffuseBsdf is too large!");

ccl_device int bsdf_principled_diffuse_setup(ccl_private PrincipledDiffuseBsdf *bsdf)
{
  bsdf->type = CLOSURE_BSDF_PRINCIPLED_DIFFUSE_ID;
  bsdf->components = PRINCIPLED_DIFFUSE_FULL;
  return SD_BSDF | SD_BSDF_HAS_EVAL;
}

ccl_device Spectrum
bsdf_principled_diffuse_compute_brdf(ccl_private const PrincipledDiffuseBsdf *bsdf,
                                     float3 N,
                                     float3 V,
                                     float3 L,
                                     ccl_private float *pdf)
{
  const float NdotL = dot(N, L);

  if (NdotL <= 0) {
    return zero_spectrum();
  }

  const float NdotV = dot(N, V);

  const float FV = schlick_fresnel(NdotV);
  const float FL = schlick_fresnel(NdotL);

  float f = 0.0f;

  /* Lambertian component. */
  if (bsdf->components & (PRINCIPLED_DIFFUSE_FULL | PRINCIPLED_DIFFUSE_LAMBERT)) {
    f += (1.0f - 0.5f * FV) * (1.0f - 0.5f * FL);
  }
  else if (bsdf->components & PRINCIPLED_DIFFUSE_LAMBERT_EXIT) {
    f += (1.0f - 0.5f * FL);
  }

  /* Retro-reflection component. */
  if (bsdf->components & (PRINCIPLED_DIFFUSE_FULL | PRINCIPLED_DIFFUSE_RETRO_REFLECTION)) {
    /* H = normalize(L + V);  // Bisector of an angle between L and V
     * LH2 = 2 * dot(L, H)^2 = 2cos(x)^2 = cos(2x) + 1 = dot(L, V) + 1,
     * half-angle x between L and V is at most 90 deg. */
    const float LH2 = dot(L, V) + 1;
    const float RR = bsdf->roughness * LH2;
    f += RR * (FL + FV + FL * FV * (RR - 1.0f));
  }

  float value = M_1_PI_F * NdotL * f;

  return make_spectrum(value);
}

/* Compute Fresnel at entry point, to be combined with #PRINCIPLED_DIFFUSE_LAMBERT_EXIT
 * at the exit point to get the complete BSDF. */
ccl_device_inline float bsdf_principled_diffuse_compute_entry_fresnel(const float NdotV)
{
  const float FV = schlick_fresnel(NdotV);
  return (1.0f - 0.5f * FV);
}

/* Ad-hoc weight adjustment to avoid retro-reflection taking away half the
 * samples from BSSRDF. */
ccl_device_inline float bsdf_principled_diffuse_retro_reflection_sample_weight(
    ccl_private PrincipledDiffuseBsdf *bsdf, const float3 I)
{
  return bsdf->roughness * schlick_fresnel(dot(bsdf->N, I));
}

ccl_device int bsdf_principled_diffuse_setup(ccl_private PrincipledDiffuseBsdf *bsdf,
                                             int components)
{
  bsdf->type = CLOSURE_BSDF_PRINCIPLED_DIFFUSE_ID;
  bsdf->components = components;
  return SD_BSDF | SD_BSDF_HAS_EVAL;
}

ccl_device Spectrum bsdf_principled_diffuse_eval_reflect(ccl_private const ShaderClosure *sc,
                                                         const float3 I,
                                                         const float3 omega_in,
                                                         ccl_private float *pdf)
{
  ccl_private const PrincipledDiffuseBsdf *bsdf = (ccl_private const PrincipledDiffuseBsdf *)sc;

  float3 N = bsdf->N;
  float3 V = I;         // outgoing
  float3 L = omega_in;  // incoming

  if (dot(N, omega_in) > 0.0f) {
    *pdf = fmaxf(dot(N, omega_in), 0.0f) * M_1_PI_F;
    return bsdf_principled_diffuse_compute_brdf(bsdf, N, V, L, pdf);
  }
  else {
    *pdf = 0.0f;
    return zero_spectrum();
  }
}

ccl_device Spectrum bsdf_principled_diffuse_eval_transmit(ccl_private const ShaderClosure *sc,
                                                          const float3 I,
                                                          const float3 omega_in,
                                                          ccl_private float *pdf)
{
  *pdf = 0.0f;
  return zero_spectrum();
}

ccl_device int bsdf_principled_diffuse_sample(ccl_private const ShaderClosure *sc,
                                              float3 Ng,
                                              float3 I,
                                              float randu,
                                              float randv,
                                              ccl_private Spectrum *eval,
                                              ccl_private float3 *omega_in,
                                              ccl_private float *pdf)
{
  ccl_private const PrincipledDiffuseBsdf *bsdf = (ccl_private const PrincipledDiffuseBsdf *)sc;

  float3 N = bsdf->N;

  sample_cos_hemisphere(N, randu, randv, omega_in, pdf);

  if (dot(Ng, *omega_in) > 0) {
    *eval = bsdf_principled_diffuse_compute_brdf(bsdf, N, I, *omega_in, pdf);
  }
  else {
    *pdf = 0.0f;
    *eval = zero_spectrum();
  }
  return LABEL_REFLECT | LABEL_DIFFUSE;
}

CCL_NAMESPACE_END
