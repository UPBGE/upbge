/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Adapted from Open Shading Language
 * Copyright (c) 2009-2010 Sony Pictures Imageworks Inc., et al.
 * All Rights Reserved.
 *
 * Modifications Copyright 2011-2022 Blender Foundation. */

#pragma once

CCL_NAMESPACE_BEGIN

typedef struct ToonBsdf {
  SHADER_CLOSURE_BASE;

  float size;
  float smooth;
} ToonBsdf;

static_assert(sizeof(ShaderClosure) >= sizeof(ToonBsdf), "ToonBsdf is too large!");

/* DIFFUSE TOON */

ccl_device int bsdf_diffuse_toon_setup(ccl_private ToonBsdf *bsdf)
{
  bsdf->type = CLOSURE_BSDF_DIFFUSE_TOON_ID;
  bsdf->size = saturatef(bsdf->size);
  bsdf->smooth = saturatef(bsdf->smooth);

  return SD_BSDF | SD_BSDF_HAS_EVAL;
}

ccl_device float bsdf_toon_get_intensity(float max_angle, float smooth, float angle)
{
  float is;

  if (angle < max_angle)
    is = 1.0f;
  else if (angle < (max_angle + smooth) && smooth != 0.0f)
    is = (1.0f - (angle - max_angle) / smooth);
  else
    is = 0.0f;

  return is;
}

ccl_device float bsdf_toon_get_sample_angle(float max_angle, float smooth)
{
  return fminf(max_angle + smooth, M_PI_2_F);
}

ccl_device Spectrum bsdf_diffuse_toon_eval_reflect(ccl_private const ShaderClosure *sc,
                                                   const float3 I,
                                                   const float3 omega_in,
                                                   ccl_private float *pdf)
{
  ccl_private const ToonBsdf *bsdf = (ccl_private const ToonBsdf *)sc;
  float max_angle = bsdf->size * M_PI_2_F;
  float smooth = bsdf->smooth * M_PI_2_F;
  float angle = safe_acosf(fmaxf(dot(bsdf->N, omega_in), 0.0f));

  float eval = bsdf_toon_get_intensity(max_angle, smooth, angle);

  if (eval > 0.0f) {
    float sample_angle = bsdf_toon_get_sample_angle(max_angle, smooth);

    *pdf = 0.5f * M_1_PI_F / (1.0f - cosf(sample_angle));
    return make_spectrum(*pdf * eval);
  }
  *pdf = 0.0f;
  return zero_spectrum();
}

ccl_device Spectrum bsdf_diffuse_toon_eval_transmit(ccl_private const ShaderClosure *sc,
                                                    const float3 I,
                                                    const float3 omega_in,
                                                    ccl_private float *pdf)
{
  *pdf = 0.0f;
  return zero_spectrum();
}

ccl_device int bsdf_diffuse_toon_sample(ccl_private const ShaderClosure *sc,
                                        float3 Ng,
                                        float3 I,
                                        float randu,
                                        float randv,
                                        ccl_private Spectrum *eval,
                                        ccl_private float3 *omega_in,
                                        ccl_private float *pdf)
{
  ccl_private const ToonBsdf *bsdf = (ccl_private const ToonBsdf *)sc;
  float max_angle = bsdf->size * M_PI_2_F;
  float smooth = bsdf->smooth * M_PI_2_F;
  float sample_angle = bsdf_toon_get_sample_angle(max_angle, smooth);
  float angle = sample_angle * randu;

  if (sample_angle > 0.0f) {
    sample_uniform_cone(bsdf->N, sample_angle, randu, randv, omega_in, pdf);

    if (dot(Ng, *omega_in) > 0.0f) {
      *eval = make_spectrum(*pdf * bsdf_toon_get_intensity(max_angle, smooth, angle));
    }
    else {
      *eval = zero_spectrum();
      *pdf = 0.0f;
    }
  }
  else {
    *eval = zero_spectrum();
    *pdf = 0.0f;
  }

  return LABEL_REFLECT | LABEL_DIFFUSE;
}

/* GLOSSY TOON */

ccl_device int bsdf_glossy_toon_setup(ccl_private ToonBsdf *bsdf)
{
  bsdf->type = CLOSURE_BSDF_GLOSSY_TOON_ID;
  bsdf->size = saturatef(bsdf->size);
  bsdf->smooth = saturatef(bsdf->smooth);

  return SD_BSDF | SD_BSDF_HAS_EVAL;
}

ccl_device Spectrum bsdf_glossy_toon_eval_reflect(ccl_private const ShaderClosure *sc,
                                                  const float3 I,
                                                  const float3 omega_in,
                                                  ccl_private float *pdf)
{
  ccl_private const ToonBsdf *bsdf = (ccl_private const ToonBsdf *)sc;
  float max_angle = bsdf->size * M_PI_2_F;
  float smooth = bsdf->smooth * M_PI_2_F;
  float cosNI = dot(bsdf->N, omega_in);
  float cosNO = dot(bsdf->N, I);

  if (cosNI > 0 && cosNO > 0) {
    /* reflect the view vector */
    float3 R = (2 * cosNO) * bsdf->N - I;
    float cosRI = dot(R, omega_in);

    float angle = safe_acosf(fmaxf(cosRI, 0.0f));

    float eval = bsdf_toon_get_intensity(max_angle, smooth, angle);
    float sample_angle = bsdf_toon_get_sample_angle(max_angle, smooth);

    *pdf = 0.5f * M_1_PI_F / (1.0f - cosf(sample_angle));
    return make_spectrum(*pdf * eval);
  }
  *pdf = 0.0f;
  return zero_spectrum();
}

ccl_device Spectrum bsdf_glossy_toon_eval_transmit(ccl_private const ShaderClosure *sc,
                                                   const float3 I,
                                                   const float3 omega_in,
                                                   ccl_private float *pdf)
{
  *pdf = 0.0f;
  return zero_spectrum();
}

ccl_device int bsdf_glossy_toon_sample(ccl_private const ShaderClosure *sc,
                                       float3 Ng,
                                       float3 I,
                                       float randu,
                                       float randv,
                                       ccl_private Spectrum *eval,
                                       ccl_private float3 *omega_in,
                                       ccl_private float *pdf)
{
  ccl_private const ToonBsdf *bsdf = (ccl_private const ToonBsdf *)sc;
  float max_angle = bsdf->size * M_PI_2_F;
  float smooth = bsdf->smooth * M_PI_2_F;
  float cosNO = dot(bsdf->N, I);

  if (cosNO > 0) {
    /* reflect the view vector */
    float3 R = (2 * cosNO) * bsdf->N - I;

    float sample_angle = bsdf_toon_get_sample_angle(max_angle, smooth);
    float angle = sample_angle * randu;

    sample_uniform_cone(R, sample_angle, randu, randv, omega_in, pdf);

    if (dot(Ng, *omega_in) > 0.0f) {
      float cosNI = dot(bsdf->N, *omega_in);

      /* make sure the direction we chose is still in the right hemisphere */
      if (cosNI > 0) {
        *eval = make_spectrum(*pdf * bsdf_toon_get_intensity(max_angle, smooth, angle));
      }
      else {
        *pdf = 0.0f;
        *eval = zero_spectrum();
      }
    }
    else {
      *pdf = 0.0f;
      *eval = zero_spectrum();
    }
  }

  return LABEL_GLOSSY | LABEL_REFLECT;
}

CCL_NAMESPACE_END
