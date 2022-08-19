/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Adapted from Open Shading Language
 * Copyright (c) 2009-2010 Sony Pictures Imageworks Inc., et al.
 * All Rights Reserved.
 *
 * Modifications Copyright 2011-2022 Blender Foundation. */

#pragma once

#include "kernel/util/color.h"

CCL_NAMESPACE_BEGIN

#ifdef __OSL__

typedef struct PhongRampBsdf {
  SHADER_CLOSURE_BASE;

  float exponent;
  ccl_private float3 *colors;
} PhongRampBsdf;

static_assert(sizeof(ShaderClosure) >= sizeof(PhongRampBsdf), "PhongRampBsdf is too large!");

ccl_device float3 bsdf_phong_ramp_get_color(const float3 colors[8], float pos)
{
  int MAXCOLORS = 8;

  float npos = pos * (float)(MAXCOLORS - 1);
  int ipos = float_to_int(npos);
  if (ipos < 0)
    return colors[0];
  if (ipos >= (MAXCOLORS - 1))
    return colors[MAXCOLORS - 1];
  float offset = npos - (float)ipos;
  return colors[ipos] * (1.0f - offset) + colors[ipos + 1] * offset;
}

ccl_device int bsdf_phong_ramp_setup(ccl_private PhongRampBsdf *bsdf)
{
  bsdf->type = CLOSURE_BSDF_PHONG_RAMP_ID;
  bsdf->exponent = max(bsdf->exponent, 0.0f);
  return SD_BSDF | SD_BSDF_HAS_EVAL;
}

ccl_device Spectrum bsdf_phong_ramp_eval_reflect(ccl_private const ShaderClosure *sc,
                                                 const float3 I,
                                                 const float3 omega_in,
                                                 ccl_private float *pdf)
{
  ccl_private const PhongRampBsdf *bsdf = (ccl_private const PhongRampBsdf *)sc;
  float m_exponent = bsdf->exponent;
  float cosNI = dot(bsdf->N, omega_in);
  float cosNO = dot(bsdf->N, I);

  if (cosNI > 0 && cosNO > 0) {
    // reflect the view vector
    float3 R = (2 * cosNO) * bsdf->N - I;
    float cosRI = dot(R, omega_in);
    if (cosRI > 0) {
      float cosp = powf(cosRI, m_exponent);
      float common = 0.5f * M_1_PI_F * cosp;
      float out = cosNI * (m_exponent + 2) * common;
      *pdf = (m_exponent + 1) * common;
      return rgb_to_spectrum(bsdf_phong_ramp_get_color(bsdf->colors, cosp) * out);
    }
  }
  *pdf = 0.0f;
  return zero_spectrum();
}

ccl_device float3 bsdf_phong_ramp_eval_transmit(ccl_private const ShaderClosure *sc,
                                                const float3 I,
                                                const float3 omega_in,
                                                ccl_private float *pdf)
{
  *pdf = 0.0f;
  return make_float3(0.0f, 0.0f, 0.0f);
}

ccl_device int bsdf_phong_ramp_sample(ccl_private const ShaderClosure *sc,
                                      float3 Ng,
                                      float3 I,
                                      float randu,
                                      float randv,
                                      ccl_private Spectrum *eval,
                                      ccl_private float3 *omega_in,
                                      ccl_private float *pdf)
{
  ccl_private const PhongRampBsdf *bsdf = (ccl_private const PhongRampBsdf *)sc;
  float cosNO = dot(bsdf->N, I);
  float m_exponent = bsdf->exponent;

  if (cosNO > 0) {
    // reflect the view vector
    float3 R = (2 * cosNO) * bsdf->N - I;
    float3 T, B;
    make_orthonormals(R, &T, &B);
    float phi = M_2PI_F * randu;
    float cosTheta = powf(randv, 1 / (m_exponent + 1));
    float sinTheta2 = 1 - cosTheta * cosTheta;
    float sinTheta = sinTheta2 > 0 ? sqrtf(sinTheta2) : 0;
    *omega_in = (cosf(phi) * sinTheta) * T + (sinf(phi) * sinTheta) * B + (cosTheta)*R;
    if (dot(Ng, *omega_in) > 0.0f) {
      // common terms for pdf and eval
      float cosNI = dot(bsdf->N, *omega_in);
      // make sure the direction we chose is still in the right hemisphere
      if (cosNI > 0) {
        float cosp = powf(cosTheta, m_exponent);
        float common = 0.5f * M_1_PI_F * cosp;
        *pdf = (m_exponent + 1) * common;
        float out = cosNI * (m_exponent + 2) * common;
        *eval = rgb_to_spectrum(bsdf_phong_ramp_get_color(bsdf->colors, cosp) * out);
      }
    }
  }
  else {
    *eval = zero_spectrum();
    *pdf = 0.0f;
  }
  return LABEL_REFLECT | LABEL_GLOSSY;
}

#endif /* __OSL__ */

CCL_NAMESPACE_END
