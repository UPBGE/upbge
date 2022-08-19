/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Adapted from Open Shading Language
 * Copyright (c) 2009-2010 Sony Pictures Imageworks Inc., et al.
 * All Rights Reserved.
 *
 * Modifications Copyright 2011-2022 Blender Foundation. */

#include <OSL/genclosure.h>
#include <OSL/oslclosure.h>

#include "kernel/osl/closures.h"
#include "kernel/osl/shader.h"

#include "util/math.h"
#include "util/param.h"

// clang-format off
#include "kernel/device/cpu/compat.h"
#include "kernel/device/cpu/globals.h"

#include "kernel/types.h"

#include "kernel/closure/alloc.h"
#include "kernel/closure/bsdf_util.h"
#include "kernel/closure/bsdf_ashikhmin_velvet.h"
#include "kernel/closure/bsdf_diffuse.h"
#include "kernel/closure/bsdf_microfacet.h"
#include "kernel/closure/bsdf_microfacet_multi.h"
#include "kernel/closure/bsdf_oren_nayar.h"
#include "kernel/closure/bsdf_reflection.h"
#include "kernel/closure/bsdf_refraction.h"
#include "kernel/closure/bsdf_transparent.h"
#include "kernel/closure/bsdf_ashikhmin_shirley.h"
#include "kernel/closure/bsdf_toon.h"
#include "kernel/closure/bsdf_hair.h"
#include "kernel/closure/bsdf_hair_principled.h"
#include "kernel/closure/bsdf_principled_diffuse.h"
#include "kernel/closure/bsdf_principled_sheen.h"
#include "kernel/closure/volume.h"

#include "kernel/util/color.h"
// clang-format on

CCL_NAMESPACE_BEGIN

using namespace OSL;

/* BSDF class definitions */

BSDF_CLOSURE_CLASS_BEGIN(Diffuse, diffuse, DiffuseBsdf, LABEL_DIFFUSE)
  BSDF_CLOSURE_FLOAT3_PARAM(DiffuseClosure, params.N)
BSDF_CLOSURE_CLASS_END(Diffuse, diffuse)

BSDF_CLOSURE_CLASS_BEGIN(Translucent, translucent, DiffuseBsdf, LABEL_DIFFUSE)
  BSDF_CLOSURE_FLOAT3_PARAM(TranslucentClosure, params.N)
BSDF_CLOSURE_CLASS_END(Translucent, translucent)

BSDF_CLOSURE_CLASS_BEGIN(OrenNayar, oren_nayar, OrenNayarBsdf, LABEL_DIFFUSE)
  BSDF_CLOSURE_FLOAT3_PARAM(OrenNayarClosure, params.N)
  BSDF_CLOSURE_FLOAT_PARAM(OrenNayarClosure, params.roughness)
BSDF_CLOSURE_CLASS_END(OrenNayar, oren_nayar)

BSDF_CLOSURE_CLASS_BEGIN(Reflection, reflection, MicrofacetBsdf, LABEL_SINGULAR)
  BSDF_CLOSURE_FLOAT3_PARAM(ReflectionClosure, params.N)
BSDF_CLOSURE_CLASS_END(Reflection, reflection)

BSDF_CLOSURE_CLASS_BEGIN(Refraction, refraction, MicrofacetBsdf, LABEL_SINGULAR)
  BSDF_CLOSURE_FLOAT3_PARAM(RefractionClosure, params.N)
  BSDF_CLOSURE_FLOAT_PARAM(RefractionClosure, params.ior)
BSDF_CLOSURE_CLASS_END(Refraction, refraction)

BSDF_CLOSURE_CLASS_BEGIN(AshikhminVelvet, ashikhmin_velvet, VelvetBsdf, LABEL_DIFFUSE)
  BSDF_CLOSURE_FLOAT3_PARAM(AshikhminVelvetClosure, params.N)
  BSDF_CLOSURE_FLOAT_PARAM(AshikhminVelvetClosure, params.sigma)
BSDF_CLOSURE_CLASS_END(AshikhminVelvet, ashikhmin_velvet)

BSDF_CLOSURE_CLASS_BEGIN(AshikhminShirley,
                         ashikhmin_shirley,
                         MicrofacetBsdf,
                         LABEL_GLOSSY | LABEL_REFLECT)
  BSDF_CLOSURE_FLOAT3_PARAM(AshikhminShirleyClosure, params.N)
  BSDF_CLOSURE_FLOAT3_PARAM(AshikhminShirleyClosure, params.T)
  BSDF_CLOSURE_FLOAT_PARAM(AshikhminShirleyClosure, params.alpha_x)
  BSDF_CLOSURE_FLOAT_PARAM(AshikhminShirleyClosure, params.alpha_y)
BSDF_CLOSURE_CLASS_END(AshikhminShirley, ashikhmin_shirley)

BSDF_CLOSURE_CLASS_BEGIN(DiffuseToon, diffuse_toon, ToonBsdf, LABEL_DIFFUSE)
  BSDF_CLOSURE_FLOAT3_PARAM(DiffuseToonClosure, params.N)
  BSDF_CLOSURE_FLOAT_PARAM(DiffuseToonClosure, params.size)
  BSDF_CLOSURE_FLOAT_PARAM(DiffuseToonClosure, params.smooth)
BSDF_CLOSURE_CLASS_END(DiffuseToon, diffuse_toon)

BSDF_CLOSURE_CLASS_BEGIN(GlossyToon, glossy_toon, ToonBsdf, LABEL_GLOSSY)
  BSDF_CLOSURE_FLOAT3_PARAM(GlossyToonClosure, params.N)
  BSDF_CLOSURE_FLOAT_PARAM(GlossyToonClosure, params.size)
  BSDF_CLOSURE_FLOAT_PARAM(GlossyToonClosure, params.smooth)
BSDF_CLOSURE_CLASS_END(GlossyToon, glossy_toon)

BSDF_CLOSURE_CLASS_BEGIN(MicrofacetGGXIsotropic,
                         microfacet_ggx_isotropic,
                         MicrofacetBsdf,
                         LABEL_GLOSSY | LABEL_REFLECT)
  BSDF_CLOSURE_FLOAT3_PARAM(MicrofacetGGXIsotropicClosure, params.N)
  BSDF_CLOSURE_FLOAT_PARAM(MicrofacetGGXIsotropicClosure, params.alpha_x)
BSDF_CLOSURE_CLASS_END(MicrofacetGGXIsotropic, microfacet_ggx_isotropic)

BSDF_CLOSURE_CLASS_BEGIN(MicrofacetGGX,
                         microfacet_ggx,
                         MicrofacetBsdf,
                         LABEL_GLOSSY | LABEL_REFLECT)
  BSDF_CLOSURE_FLOAT3_PARAM(MicrofacetGGXClosure, params.N)
  BSDF_CLOSURE_FLOAT3_PARAM(MicrofacetGGXClosure, params.T)
  BSDF_CLOSURE_FLOAT_PARAM(MicrofacetGGXClosure, params.alpha_x)
  BSDF_CLOSURE_FLOAT_PARAM(MicrofacetGGXClosure, params.alpha_y)
BSDF_CLOSURE_CLASS_END(MicrofacetGGX, microfacet_ggx)

BSDF_CLOSURE_CLASS_BEGIN(MicrofacetBeckmannIsotropic,
                         microfacet_beckmann_isotropic,
                         MicrofacetBsdf,
                         LABEL_GLOSSY | LABEL_REFLECT)
  BSDF_CLOSURE_FLOAT3_PARAM(MicrofacetBeckmannIsotropicClosure, params.N)
  BSDF_CLOSURE_FLOAT_PARAM(MicrofacetBeckmannIsotropicClosure, params.alpha_x)
BSDF_CLOSURE_CLASS_END(MicrofacetBeckmannIsotropic, microfacet_beckmann_isotropic)

BSDF_CLOSURE_CLASS_BEGIN(MicrofacetBeckmann,
                         microfacet_beckmann,
                         MicrofacetBsdf,
                         LABEL_GLOSSY | LABEL_REFLECT)
  BSDF_CLOSURE_FLOAT3_PARAM(MicrofacetBeckmannClosure, params.N)
  BSDF_CLOSURE_FLOAT3_PARAM(MicrofacetBeckmannClosure, params.T)
  BSDF_CLOSURE_FLOAT_PARAM(MicrofacetBeckmannClosure, params.alpha_x)
  BSDF_CLOSURE_FLOAT_PARAM(MicrofacetBeckmannClosure, params.alpha_y)
BSDF_CLOSURE_CLASS_END(MicrofacetBeckmann, microfacet_beckmann)

BSDF_CLOSURE_CLASS_BEGIN(MicrofacetGGXRefraction,
                         microfacet_ggx_refraction,
                         MicrofacetBsdf,
                         LABEL_GLOSSY | LABEL_TRANSMIT)
  BSDF_CLOSURE_FLOAT3_PARAM(MicrofacetGGXRefractionClosure, params.N)
  BSDF_CLOSURE_FLOAT_PARAM(MicrofacetGGXRefractionClosure, params.alpha_x)
  BSDF_CLOSURE_FLOAT_PARAM(MicrofacetGGXRefractionClosure, params.ior)
BSDF_CLOSURE_CLASS_END(MicrofacetGGXRefraction, microfacet_ggx_refraction)

BSDF_CLOSURE_CLASS_BEGIN(MicrofacetBeckmannRefraction,
                         microfacet_beckmann_refraction,
                         MicrofacetBsdf,
                         LABEL_GLOSSY | LABEL_TRANSMIT)
  BSDF_CLOSURE_FLOAT3_PARAM(MicrofacetBeckmannRefractionClosure, params.N)
  BSDF_CLOSURE_FLOAT_PARAM(MicrofacetBeckmannRefractionClosure, params.alpha_x)
  BSDF_CLOSURE_FLOAT_PARAM(MicrofacetBeckmannRefractionClosure, params.ior)
BSDF_CLOSURE_CLASS_END(MicrofacetBeckmannRefraction, microfacet_beckmann_refraction)

BSDF_CLOSURE_CLASS_BEGIN(HairReflection, hair_reflection, HairBsdf, LABEL_GLOSSY)
  BSDF_CLOSURE_FLOAT3_PARAM(HairReflectionClosure, params.N)
  BSDF_CLOSURE_FLOAT_PARAM(HairReflectionClosure, params.roughness1)
  BSDF_CLOSURE_FLOAT_PARAM(HairReflectionClosure, params.roughness2)
  BSDF_CLOSURE_FLOAT3_PARAM(HairReflectionClosure, params.T)
  BSDF_CLOSURE_FLOAT_PARAM(HairReflectionClosure, params.offset)
BSDF_CLOSURE_CLASS_END(HairReflection, hair_reflection)

BSDF_CLOSURE_CLASS_BEGIN(HairTransmission, hair_transmission, HairBsdf, LABEL_GLOSSY)
  BSDF_CLOSURE_FLOAT3_PARAM(HairTransmissionClosure, params.N)
  BSDF_CLOSURE_FLOAT_PARAM(HairTransmissionClosure, params.roughness1)
  BSDF_CLOSURE_FLOAT_PARAM(HairTransmissionClosure, params.roughness2)
  BSDF_CLOSURE_FLOAT3_PARAM(HairReflectionClosure, params.T)
  BSDF_CLOSURE_FLOAT_PARAM(HairReflectionClosure, params.offset)
BSDF_CLOSURE_CLASS_END(HairTransmission, hair_transmission)

BSDF_CLOSURE_CLASS_BEGIN(PrincipledDiffuse,
                         principled_diffuse,
                         PrincipledDiffuseBsdf,
                         LABEL_DIFFUSE)
  BSDF_CLOSURE_FLOAT3_PARAM(PrincipledDiffuseClosure, params.N)
  BSDF_CLOSURE_FLOAT_PARAM(PrincipledDiffuseClosure, params.roughness)
BSDF_CLOSURE_CLASS_END(PrincipledDiffuse, principled_diffuse)

class PrincipledSheenClosure : public CBSDFClosure {
 public:
  PrincipledSheenBsdf params;

  void setup(ShaderData *sd, uint32_t path_flag, float3 weight)
  {
    if (!skip(sd, path_flag, LABEL_DIFFUSE)) {
      params.N = ensure_valid_reflection(sd->Ng, sd->I, params.N);

      PrincipledSheenBsdf *bsdf = (PrincipledSheenBsdf *)bsdf_alloc_osl(
          sd, sizeof(PrincipledSheenBsdf), rgb_to_spectrum(weight), &params);
      sd->flag |= (bsdf) ? bsdf_principled_sheen_setup(sd, bsdf) : 0;
    }
  }
};

static ClosureParam *bsdf_principled_sheen_params()
{
  static ClosureParam params[] = {CLOSURE_FLOAT3_PARAM(PrincipledSheenClosure, params.N),
                                  CLOSURE_STRING_KEYPARAM(PrincipledSheenClosure, label, "label"),
                                  CLOSURE_FINISH_PARAM(PrincipledSheenClosure)};
  return params;
}

CCLOSURE_PREPARE_STATIC(closure_bsdf_principled_sheen_prepare, PrincipledSheenClosure)

/* PRINCIPLED HAIR BSDF */
class PrincipledHairClosure : public CBSDFClosure {
 public:
  PrincipledHairBSDF params;

  PrincipledHairBSDF *alloc(ShaderData *sd, uint32_t path_flag, float3 weight)
  {
    PrincipledHairBSDF *bsdf = (PrincipledHairBSDF *)bsdf_alloc_osl(
        sd, sizeof(PrincipledHairBSDF), rgb_to_spectrum(weight), &params);
    if (!bsdf) {
      return NULL;
    }

    PrincipledHairExtra *extra = (PrincipledHairExtra *)closure_alloc_extra(
        sd, sizeof(PrincipledHairExtra));
    if (!extra) {
      return NULL;
    }

    bsdf->extra = extra;
    return bsdf;
  }

  void setup(ShaderData *sd, uint32_t path_flag, float3 weight)
  {
    if (!skip(sd, path_flag, LABEL_GLOSSY)) {
      params.N = ensure_valid_reflection(sd->Ng, sd->I, params.N);

      PrincipledHairBSDF *bsdf = (PrincipledHairBSDF *)alloc(sd, path_flag, weight);
      if (!bsdf) {
        return;
      }

      sd->flag |= (bsdf) ? bsdf_principled_hair_setup(sd, bsdf) : 0;
    }
  }
};

static ClosureParam *closure_bsdf_principled_hair_params()
{
  static ClosureParam params[] = {CLOSURE_FLOAT3_PARAM(PrincipledHairClosure, params.N),
                                  CLOSURE_FLOAT3_PARAM(PrincipledHairClosure, params.sigma),
                                  CLOSURE_FLOAT_PARAM(PrincipledHairClosure, params.v),
                                  CLOSURE_FLOAT_PARAM(PrincipledHairClosure, params.s),
                                  CLOSURE_FLOAT_PARAM(PrincipledHairClosure, params.m0_roughness),
                                  CLOSURE_FLOAT_PARAM(PrincipledHairClosure, params.alpha),
                                  CLOSURE_FLOAT_PARAM(PrincipledHairClosure, params.eta),
                                  CLOSURE_STRING_KEYPARAM(PrincipledHairClosure, label, "label"),
                                  CLOSURE_FINISH_PARAM(PrincipledHairClosure)};

  return params;
}

CCLOSURE_PREPARE(closure_bsdf_principled_hair_prepare, PrincipledHairClosure)

/* DISNEY PRINCIPLED CLEARCOAT */
class PrincipledClearcoatClosure : public CBSDFClosure {
 public:
  MicrofacetBsdf params;
  float clearcoat, clearcoat_roughness;

  MicrofacetBsdf *alloc(ShaderData *sd, uint32_t path_flag, float3 weight)
  {
    MicrofacetBsdf *bsdf = (MicrofacetBsdf *)bsdf_alloc_osl(
        sd, sizeof(MicrofacetBsdf), rgb_to_spectrum(weight), &params);
    if (!bsdf) {
      return NULL;
    }

    MicrofacetExtra *extra = (MicrofacetExtra *)closure_alloc_extra(sd, sizeof(MicrofacetExtra));
    if (!extra) {
      return NULL;
    }

    bsdf->T = zero_float3();
    bsdf->extra = extra;
    bsdf->ior = 1.5f;
    bsdf->alpha_x = clearcoat_roughness;
    bsdf->alpha_y = clearcoat_roughness;
    bsdf->extra->color = zero_spectrum();
    bsdf->extra->cspec0 = make_spectrum(0.04f);
    bsdf->extra->clearcoat = clearcoat;
    return bsdf;
  }

  void setup(ShaderData *sd, uint32_t path_flag, float3 weight)
  {
    params.N = ensure_valid_reflection(sd->Ng, sd->I, params.N);
    MicrofacetBsdf *bsdf = alloc(sd, path_flag, weight);
    if (!bsdf) {
      return;
    }

    sd->flag |= bsdf_microfacet_ggx_clearcoat_setup(bsdf, sd);
  }
};

ClosureParam *closure_bsdf_principled_clearcoat_params()
{
  static ClosureParam params[] = {
      CLOSURE_FLOAT3_PARAM(PrincipledClearcoatClosure, params.N),
      CLOSURE_FLOAT_PARAM(PrincipledClearcoatClosure, clearcoat),
      CLOSURE_FLOAT_PARAM(PrincipledClearcoatClosure, clearcoat_roughness),
      CLOSURE_STRING_KEYPARAM(PrincipledClearcoatClosure, label, "label"),
      CLOSURE_FINISH_PARAM(PrincipledClearcoatClosure)};
  return params;
}
CCLOSURE_PREPARE(closure_bsdf_principled_clearcoat_prepare, PrincipledClearcoatClosure)

/* Registration */

static void register_closure(OSL::ShadingSystem *ss,
                             const char *name,
                             int id,
                             OSL::ClosureParam *params,
                             OSL::PrepareClosureFunc prepare)
{
  /* optimization: it's possible to not use a prepare function at all and
   * only initialize the actual class when accessing the closure component
   * data, but then we need to map the id to the class somehow */
#if OSL_LIBRARY_VERSION_CODE >= 10900
  ss->register_closure(name, id, params, prepare, NULL);
#else
  ss->register_closure(name, id, params, prepare, NULL, 16);
#endif
}

void OSLShader::register_closures(OSLShadingSystem *ss_)
{
  OSL::ShadingSystem *ss = (OSL::ShadingSystem *)ss_;
  int id = 0;

  register_closure(ss, "diffuse", id++, bsdf_diffuse_params(), bsdf_diffuse_prepare);
  register_closure(ss, "oren_nayar", id++, bsdf_oren_nayar_params(), bsdf_oren_nayar_prepare);
  register_closure(ss, "translucent", id++, bsdf_translucent_params(), bsdf_translucent_prepare);
  register_closure(ss, "reflection", id++, bsdf_reflection_params(), bsdf_reflection_prepare);
  register_closure(ss, "refraction", id++, bsdf_refraction_params(), bsdf_refraction_prepare);
  register_closure(ss,
                   "transparent",
                   id++,
                   closure_bsdf_transparent_params(),
                   closure_bsdf_transparent_prepare);

  register_closure(
      ss, "microfacet", id++, closure_bsdf_microfacet_params(), closure_bsdf_microfacet_prepare);
  register_closure(ss,
                   "microfacet_ggx",
                   id++,
                   bsdf_microfacet_ggx_isotropic_params(),
                   bsdf_microfacet_ggx_isotropic_prepare);
  register_closure(
      ss, "microfacet_ggx_aniso", id++, bsdf_microfacet_ggx_params(), bsdf_microfacet_ggx_prepare);
  register_closure(ss,
                   "microfacet_ggx_refraction",
                   id++,
                   bsdf_microfacet_ggx_refraction_params(),
                   bsdf_microfacet_ggx_refraction_prepare);
  register_closure(ss,
                   "microfacet_multi_ggx",
                   id++,
                   closure_bsdf_microfacet_multi_ggx_params(),
                   closure_bsdf_microfacet_multi_ggx_prepare);
  register_closure(ss,
                   "microfacet_multi_ggx_glass",
                   id++,
                   closure_bsdf_microfacet_multi_ggx_glass_params(),
                   closure_bsdf_microfacet_multi_ggx_glass_prepare);
  register_closure(ss,
                   "microfacet_multi_ggx_aniso",
                   id++,
                   closure_bsdf_microfacet_multi_ggx_aniso_params(),
                   closure_bsdf_microfacet_multi_ggx_aniso_prepare);
  register_closure(ss,
                   "microfacet_ggx_fresnel",
                   id++,
                   closure_bsdf_microfacet_ggx_fresnel_params(),
                   closure_bsdf_microfacet_ggx_fresnel_prepare);
  register_closure(ss,
                   "microfacet_ggx_aniso_fresnel",
                   id++,
                   closure_bsdf_microfacet_ggx_aniso_fresnel_params(),
                   closure_bsdf_microfacet_ggx_aniso_fresnel_prepare);
  register_closure(ss,
                   "microfacet_multi_ggx_fresnel",
                   id++,
                   closure_bsdf_microfacet_multi_ggx_fresnel_params(),
                   closure_bsdf_microfacet_multi_ggx_fresnel_prepare);
  register_closure(ss,
                   "microfacet_multi_ggx_glass_fresnel",
                   id++,
                   closure_bsdf_microfacet_multi_ggx_glass_fresnel_params(),
                   closure_bsdf_microfacet_multi_ggx_glass_fresnel_prepare);
  register_closure(ss,
                   "microfacet_multi_ggx_aniso_fresnel",
                   id++,
                   closure_bsdf_microfacet_multi_ggx_aniso_fresnel_params(),
                   closure_bsdf_microfacet_multi_ggx_aniso_fresnel_prepare);
  register_closure(ss,
                   "microfacet_beckmann",
                   id++,
                   bsdf_microfacet_beckmann_isotropic_params(),
                   bsdf_microfacet_beckmann_isotropic_prepare);
  register_closure(ss,
                   "microfacet_beckmann_aniso",
                   id++,
                   bsdf_microfacet_beckmann_params(),
                   bsdf_microfacet_beckmann_prepare);
  register_closure(ss,
                   "microfacet_beckmann_refraction",
                   id++,
                   bsdf_microfacet_beckmann_refraction_params(),
                   bsdf_microfacet_beckmann_refraction_prepare);
  register_closure(ss,
                   "ashikhmin_shirley",
                   id++,
                   bsdf_ashikhmin_shirley_params(),
                   bsdf_ashikhmin_shirley_prepare);
  register_closure(
      ss, "ashikhmin_velvet", id++, bsdf_ashikhmin_velvet_params(), bsdf_ashikhmin_velvet_prepare);
  register_closure(
      ss, "diffuse_toon", id++, bsdf_diffuse_toon_params(), bsdf_diffuse_toon_prepare);
  register_closure(ss, "glossy_toon", id++, bsdf_glossy_toon_params(), bsdf_glossy_toon_prepare);
  register_closure(ss,
                   "principled_diffuse",
                   id++,
                   bsdf_principled_diffuse_params(),
                   bsdf_principled_diffuse_prepare);
  register_closure(ss,
                   "principled_sheen",
                   id++,
                   bsdf_principled_sheen_params(),
                   closure_bsdf_principled_sheen_prepare);
  register_closure(ss,
                   "principled_clearcoat",
                   id++,
                   closure_bsdf_principled_clearcoat_params(),
                   closure_bsdf_principled_clearcoat_prepare);

  register_closure(ss, "emission", id++, closure_emission_params(), closure_emission_prepare);
  register_closure(
      ss, "background", id++, closure_background_params(), closure_background_prepare);
  register_closure(ss, "holdout", id++, closure_holdout_params(), closure_holdout_prepare);
  register_closure(ss,
                   "diffuse_ramp",
                   id++,
                   closure_bsdf_diffuse_ramp_params(),
                   closure_bsdf_diffuse_ramp_prepare);
  register_closure(
      ss, "phong_ramp", id++, closure_bsdf_phong_ramp_params(), closure_bsdf_phong_ramp_prepare);
  register_closure(ss, "bssrdf", id++, closure_bssrdf_params(), closure_bssrdf_prepare);

  register_closure(
      ss, "hair_reflection", id++, bsdf_hair_reflection_params(), bsdf_hair_reflection_prepare);
  register_closure(ss,
                   "hair_transmission",
                   id++,
                   bsdf_hair_transmission_params(),
                   bsdf_hair_transmission_prepare);

  register_closure(ss,
                   "principled_hair",
                   id++,
                   closure_bsdf_principled_hair_params(),
                   closure_bsdf_principled_hair_prepare);

  register_closure(ss,
                   "henyey_greenstein",
                   id++,
                   closure_henyey_greenstein_params(),
                   closure_henyey_greenstein_prepare);
  register_closure(
      ss, "absorption", id++, closure_absorption_params(), closure_absorption_prepare);
}

/* BSDF Closure */

bool CBSDFClosure::skip(const ShaderData *sd, uint32_t path_flag, int scattering)
{
  /* caustic options */
  if ((scattering & LABEL_GLOSSY) && (path_flag & PATH_RAY_DIFFUSE)) {
    const KernelGlobalsCPU *kg = sd->osl_globals;

    if ((!kernel_data.integrator.caustics_reflective && (scattering & LABEL_REFLECT)) ||
        (!kernel_data.integrator.caustics_refractive && (scattering & LABEL_TRANSMIT))) {
      return true;
    }
  }

  return false;
}

/* Standard Microfacet Closure */

class MicrofacetClosure : public CBSDFClosure {
 public:
  MicrofacetBsdf params;
  ustring distribution;
  int refract;

  void setup(ShaderData *sd, uint32_t path_flag, float3 weight)
  {
    static ustring u_ggx("ggx");
    static ustring u_default("default");

    const int label = (refract) ? LABEL_TRANSMIT : LABEL_REFLECT;
    if (skip(sd, path_flag, LABEL_GLOSSY | label)) {
      return;
    }

    params.N = ensure_valid_reflection(sd->Ng, sd->I, params.N);

    MicrofacetBsdf *bsdf = (MicrofacetBsdf *)bsdf_alloc_osl(
        sd, sizeof(MicrofacetBsdf), rgb_to_spectrum(weight), &params);

    if (!bsdf) {
      return;
    }

    /* GGX */
    if (distribution == u_ggx || distribution == u_default) {
      if (!refract) {
        if (params.alpha_x == params.alpha_y) {
          /* Isotropic */
          sd->flag |= bsdf_microfacet_ggx_isotropic_setup(bsdf);
        }
        else {
          /* Anisotropic */
          sd->flag |= bsdf_microfacet_ggx_setup(bsdf);
        }
      }
      else {
        sd->flag |= bsdf_microfacet_ggx_refraction_setup(bsdf);
      }
    }
    /* Beckmann */
    else {
      if (!refract) {
        if (params.alpha_x == params.alpha_y) {
          /* Isotropic */
          sd->flag |= bsdf_microfacet_beckmann_isotropic_setup(bsdf);
        }
        else {
          /* Anisotropic */
          sd->flag |= bsdf_microfacet_beckmann_setup(bsdf);
        }
      }
      else {
        sd->flag |= bsdf_microfacet_beckmann_refraction_setup(bsdf);
      }
    }
  }
};

ClosureParam *closure_bsdf_microfacet_params()
{
  static ClosureParam params[] = {CLOSURE_STRING_PARAM(MicrofacetClosure, distribution),
                                  CLOSURE_FLOAT3_PARAM(MicrofacetClosure, params.N),
                                  CLOSURE_FLOAT3_PARAM(MicrofacetClosure, params.T),
                                  CLOSURE_FLOAT_PARAM(MicrofacetClosure, params.alpha_x),
                                  CLOSURE_FLOAT_PARAM(MicrofacetClosure, params.alpha_y),
                                  CLOSURE_FLOAT_PARAM(MicrofacetClosure, params.ior),
                                  CLOSURE_INT_PARAM(MicrofacetClosure, refract),
                                  CLOSURE_STRING_KEYPARAM(MicrofacetClosure, label, "label"),
                                  CLOSURE_FINISH_PARAM(MicrofacetClosure)};

  return params;
}
CCLOSURE_PREPARE(closure_bsdf_microfacet_prepare, MicrofacetClosure)

/* GGX closures with Fresnel */

class MicrofacetFresnelClosure : public CBSDFClosure {
 public:
  MicrofacetBsdf params;
  float3 color;
  float3 cspec0;

  MicrofacetBsdf *alloc(ShaderData *sd, uint32_t path_flag, float3 weight)
  {
    /* Technically, the MultiGGX Glass closure may also transmit. However,
     * since this is set statically and only used for caustic flags, this
     * is probably as good as it gets. */
    if (skip(sd, path_flag, LABEL_GLOSSY | LABEL_REFLECT)) {
      return NULL;
    }

    MicrofacetBsdf *bsdf = (MicrofacetBsdf *)bsdf_alloc_osl(
        sd, sizeof(MicrofacetBsdf), rgb_to_spectrum(weight), &params);
    if (!bsdf) {
      return NULL;
    }

    MicrofacetExtra *extra = (MicrofacetExtra *)closure_alloc_extra(sd, sizeof(MicrofacetExtra));
    if (!extra) {
      return NULL;
    }

    bsdf->extra = extra;
    bsdf->extra->color = rgb_to_spectrum(color);
    bsdf->extra->cspec0 = rgb_to_spectrum(cspec0);
    bsdf->extra->clearcoat = 0.0f;
    return bsdf;
  }
};

class MicrofacetGGXFresnelClosure : public MicrofacetFresnelClosure {
 public:
  void setup(ShaderData *sd, uint32_t path_flag, float3 weight)
  {
    params.N = ensure_valid_reflection(sd->Ng, sd->I, params.N);

    MicrofacetBsdf *bsdf = alloc(sd, path_flag, weight);
    if (!bsdf) {
      return;
    }

    bsdf->T = zero_float3();
    bsdf->alpha_y = bsdf->alpha_x;
    sd->flag |= bsdf_microfacet_ggx_fresnel_setup(bsdf, sd);
  }
};

ClosureParam *closure_bsdf_microfacet_ggx_fresnel_params()
{
  static ClosureParam params[] = {
      CLOSURE_FLOAT3_PARAM(MicrofacetGGXFresnelClosure, params.N),
      CLOSURE_FLOAT_PARAM(MicrofacetGGXFresnelClosure, params.alpha_x),
      CLOSURE_FLOAT_PARAM(MicrofacetGGXFresnelClosure, params.ior),
      CLOSURE_FLOAT3_PARAM(MicrofacetGGXFresnelClosure, color),
      CLOSURE_FLOAT3_PARAM(MicrofacetGGXFresnelClosure, cspec0),
      CLOSURE_STRING_KEYPARAM(MicrofacetGGXFresnelClosure, label, "label"),
      CLOSURE_FINISH_PARAM(MicrofacetGGXFresnelClosure)};
  return params;
}
CCLOSURE_PREPARE(closure_bsdf_microfacet_ggx_fresnel_prepare, MicrofacetGGXFresnelClosure);

class MicrofacetGGXAnisoFresnelClosure : public MicrofacetFresnelClosure {
 public:
  void setup(ShaderData *sd, uint32_t path_flag, float3 weight)
  {
    params.N = ensure_valid_reflection(sd->Ng, sd->I, params.N);

    MicrofacetBsdf *bsdf = alloc(sd, path_flag, weight);
    if (!bsdf) {
      return;
    }

    sd->flag |= bsdf_microfacet_ggx_fresnel_setup(bsdf, sd);
  }
};

ClosureParam *closure_bsdf_microfacet_ggx_aniso_fresnel_params()
{
  static ClosureParam params[] = {
      CLOSURE_FLOAT3_PARAM(MicrofacetGGXFresnelClosure, params.N),
      CLOSURE_FLOAT3_PARAM(MicrofacetGGXFresnelClosure, params.T),
      CLOSURE_FLOAT_PARAM(MicrofacetGGXFresnelClosure, params.alpha_x),
      CLOSURE_FLOAT_PARAM(MicrofacetGGXFresnelClosure, params.alpha_y),
      CLOSURE_FLOAT_PARAM(MicrofacetGGXFresnelClosure, params.ior),
      CLOSURE_FLOAT3_PARAM(MicrofacetGGXFresnelClosure, color),
      CLOSURE_FLOAT3_PARAM(MicrofacetGGXFresnelClosure, cspec0),
      CLOSURE_STRING_KEYPARAM(MicrofacetGGXFresnelClosure, label, "label"),
      CLOSURE_FINISH_PARAM(MicrofacetGGXFresnelClosure)};
  return params;
}
CCLOSURE_PREPARE(closure_bsdf_microfacet_ggx_aniso_fresnel_prepare,
                 MicrofacetGGXAnisoFresnelClosure);

/* Multiscattering GGX closures */

class MicrofacetMultiClosure : public CBSDFClosure {
 public:
  MicrofacetBsdf params;
  float3 color;

  MicrofacetBsdf *alloc(ShaderData *sd, uint32_t path_flag, float3 weight)
  {
    /* Technically, the MultiGGX closure may also transmit. However,
     * since this is set statically and only used for caustic flags, this
     * is probably as good as it gets. */
    if (skip(sd, path_flag, LABEL_GLOSSY | LABEL_REFLECT)) {
      return NULL;
    }

    MicrofacetBsdf *bsdf = (MicrofacetBsdf *)bsdf_alloc_osl(
        sd, sizeof(MicrofacetBsdf), rgb_to_spectrum(weight), &params);
    if (!bsdf) {
      return NULL;
    }

    MicrofacetExtra *extra = (MicrofacetExtra *)closure_alloc_extra(sd, sizeof(MicrofacetExtra));
    if (!extra) {
      return NULL;
    }

    bsdf->extra = extra;
    bsdf->extra->color = rgb_to_spectrum(color);
    bsdf->extra->cspec0 = zero_spectrum();
    bsdf->extra->clearcoat = 0.0f;
    return bsdf;
  }
};

class MicrofacetMultiGGXClosure : public MicrofacetMultiClosure {
 public:
  void setup(ShaderData *sd, uint32_t path_flag, float3 weight)
  {
    params.N = ensure_valid_reflection(sd->Ng, sd->I, params.N);

    MicrofacetBsdf *bsdf = alloc(sd, path_flag, weight);
    if (!bsdf) {
      return;
    }

    bsdf->ior = 0.0f;
    bsdf->T = zero_float3();
    bsdf->alpha_y = bsdf->alpha_x;
    sd->flag |= bsdf_microfacet_multi_ggx_setup(bsdf);
  }
};

ClosureParam *closure_bsdf_microfacet_multi_ggx_params()
{
  static ClosureParam params[] = {
      CLOSURE_FLOAT3_PARAM(MicrofacetMultiGGXClosure, params.N),
      CLOSURE_FLOAT_PARAM(MicrofacetMultiGGXClosure, params.alpha_x),
      CLOSURE_FLOAT3_PARAM(MicrofacetMultiGGXClosure, color),
      CLOSURE_STRING_KEYPARAM(MicrofacetMultiGGXClosure, label, "label"),
      CLOSURE_FINISH_PARAM(MicrofacetMultiGGXClosure)};
  return params;
}
CCLOSURE_PREPARE(closure_bsdf_microfacet_multi_ggx_prepare, MicrofacetMultiGGXClosure);

class MicrofacetMultiGGXAnisoClosure : public MicrofacetMultiClosure {
 public:
  void setup(ShaderData *sd, uint32_t path_flag, float3 weight)
  {
    params.N = ensure_valid_reflection(sd->Ng, sd->I, params.N);

    MicrofacetBsdf *bsdf = alloc(sd, path_flag, weight);
    if (!bsdf) {
      return;
    }

    bsdf->ior = 0.0f;
    sd->flag |= bsdf_microfacet_multi_ggx_setup(bsdf);
  }
};

ClosureParam *closure_bsdf_microfacet_multi_ggx_aniso_params()
{
  static ClosureParam params[] = {
      CLOSURE_FLOAT3_PARAM(MicrofacetMultiGGXClosure, params.N),
      CLOSURE_FLOAT3_PARAM(MicrofacetMultiGGXClosure, params.T),
      CLOSURE_FLOAT_PARAM(MicrofacetMultiGGXClosure, params.alpha_x),
      CLOSURE_FLOAT_PARAM(MicrofacetMultiGGXClosure, params.alpha_y),
      CLOSURE_FLOAT3_PARAM(MicrofacetMultiGGXClosure, color),
      CLOSURE_STRING_KEYPARAM(MicrofacetMultiGGXClosure, label, "label"),
      CLOSURE_FINISH_PARAM(MicrofacetMultiGGXClosure)};
  return params;
}
CCLOSURE_PREPARE(closure_bsdf_microfacet_multi_ggx_aniso_prepare, MicrofacetMultiGGXAnisoClosure);

class MicrofacetMultiGGXGlassClosure : public MicrofacetMultiClosure {
 public:
  MicrofacetMultiGGXGlassClosure() : MicrofacetMultiClosure()
  {
  }

  void setup(ShaderData *sd, uint32_t path_flag, float3 weight)
  {
    params.N = ensure_valid_reflection(sd->Ng, sd->I, params.N);

    MicrofacetBsdf *bsdf = alloc(sd, path_flag, weight);
    if (!bsdf) {
      return;
    }

    bsdf->T = zero_float3();
    bsdf->alpha_y = bsdf->alpha_x;
    sd->flag |= bsdf_microfacet_multi_ggx_glass_setup(bsdf);
  }
};

ClosureParam *closure_bsdf_microfacet_multi_ggx_glass_params()
{
  static ClosureParam params[] = {
      CLOSURE_FLOAT3_PARAM(MicrofacetMultiGGXClosure, params.N),
      CLOSURE_FLOAT_PARAM(MicrofacetMultiGGXClosure, params.alpha_x),
      CLOSURE_FLOAT_PARAM(MicrofacetMultiGGXClosure, params.ior),
      CLOSURE_FLOAT3_PARAM(MicrofacetMultiGGXClosure, color),
      CLOSURE_STRING_KEYPARAM(MicrofacetMultiGGXClosure, label, "label"),
      CLOSURE_FINISH_PARAM(MicrofacetMultiGGXClosure)};
  return params;
}
CCLOSURE_PREPARE(closure_bsdf_microfacet_multi_ggx_glass_prepare, MicrofacetMultiGGXGlassClosure);

/* Multiscattering GGX closures with Fresnel */

class MicrofacetMultiFresnelClosure : public CBSDFClosure {
 public:
  MicrofacetBsdf params;
  float3 color;
  float3 cspec0;

  MicrofacetBsdf *alloc(ShaderData *sd, uint32_t path_flag, float3 weight)
  {
    /* Technically, the MultiGGX closure may also transmit. However,
     * since this is set statically and only used for caustic flags, this
     * is probably as good as it gets. */
    if (skip(sd, path_flag, LABEL_GLOSSY | LABEL_REFLECT)) {
      return NULL;
    }

    MicrofacetBsdf *bsdf = (MicrofacetBsdf *)bsdf_alloc_osl(
        sd, sizeof(MicrofacetBsdf), rgb_to_spectrum(weight), &params);
    if (!bsdf) {
      return NULL;
    }

    MicrofacetExtra *extra = (MicrofacetExtra *)closure_alloc_extra(sd, sizeof(MicrofacetExtra));
    if (!extra) {
      return NULL;
    }

    bsdf->extra = extra;
    bsdf->extra->color = rgb_to_spectrum(color);
    bsdf->extra->cspec0 = rgb_to_spectrum(cspec0);
    bsdf->extra->clearcoat = 0.0f;
    return bsdf;
  }
};

class MicrofacetMultiGGXFresnelClosure : public MicrofacetMultiFresnelClosure {
 public:
  void setup(ShaderData *sd, uint32_t path_flag, float3 weight)
  {
    params.N = ensure_valid_reflection(sd->Ng, sd->I, params.N);

    MicrofacetBsdf *bsdf = alloc(sd, path_flag, weight);
    if (!bsdf) {
      return;
    }

    bsdf->T = zero_float3();
    bsdf->alpha_y = bsdf->alpha_x;
    sd->flag |= bsdf_microfacet_multi_ggx_fresnel_setup(bsdf, sd);
  }
};

ClosureParam *closure_bsdf_microfacet_multi_ggx_fresnel_params()
{
  static ClosureParam params[] = {
      CLOSURE_FLOAT3_PARAM(MicrofacetMultiGGXFresnelClosure, params.N),
      CLOSURE_FLOAT_PARAM(MicrofacetMultiGGXFresnelClosure, params.alpha_x),
      CLOSURE_FLOAT_PARAM(MicrofacetMultiGGXFresnelClosure, params.ior),
      CLOSURE_FLOAT3_PARAM(MicrofacetMultiGGXFresnelClosure, color),
      CLOSURE_FLOAT3_PARAM(MicrofacetMultiGGXFresnelClosure, cspec0),
      CLOSURE_STRING_KEYPARAM(MicrofacetMultiGGXFresnelClosure, label, "label"),
      CLOSURE_FINISH_PARAM(MicrofacetMultiGGXFresnelClosure)};
  return params;
}
CCLOSURE_PREPARE(closure_bsdf_microfacet_multi_ggx_fresnel_prepare,
                 MicrofacetMultiGGXFresnelClosure);

class MicrofacetMultiGGXAnisoFresnelClosure : public MicrofacetMultiFresnelClosure {
 public:
  void setup(ShaderData *sd, uint32_t path_flag, float3 weight)
  {
    params.N = ensure_valid_reflection(sd->Ng, sd->I, params.N);

    MicrofacetBsdf *bsdf = alloc(sd, path_flag, weight);
    if (!bsdf) {
      return;
    }

    sd->flag |= bsdf_microfacet_multi_ggx_fresnel_setup(bsdf, sd);
  }
};

ClosureParam *closure_bsdf_microfacet_multi_ggx_aniso_fresnel_params()
{
  static ClosureParam params[] = {
      CLOSURE_FLOAT3_PARAM(MicrofacetMultiGGXFresnelClosure, params.N),
      CLOSURE_FLOAT3_PARAM(MicrofacetMultiGGXFresnelClosure, params.T),
      CLOSURE_FLOAT_PARAM(MicrofacetMultiGGXFresnelClosure, params.alpha_x),
      CLOSURE_FLOAT_PARAM(MicrofacetMultiGGXFresnelClosure, params.alpha_y),
      CLOSURE_FLOAT_PARAM(MicrofacetMultiGGXFresnelClosure, params.ior),
      CLOSURE_FLOAT3_PARAM(MicrofacetMultiGGXFresnelClosure, color),
      CLOSURE_FLOAT3_PARAM(MicrofacetMultiGGXFresnelClosure, cspec0),
      CLOSURE_STRING_KEYPARAM(MicrofacetMultiGGXFresnelClosure, label, "label"),
      CLOSURE_FINISH_PARAM(MicrofacetMultiGGXFresnelClosure)};
  return params;
}
CCLOSURE_PREPARE(closure_bsdf_microfacet_multi_ggx_aniso_fresnel_prepare,
                 MicrofacetMultiGGXAnisoFresnelClosure);

class MicrofacetMultiGGXGlassFresnelClosure : public MicrofacetMultiFresnelClosure {
 public:
  MicrofacetMultiGGXGlassFresnelClosure() : MicrofacetMultiFresnelClosure()
  {
  }

  void setup(ShaderData *sd, uint32_t path_flag, float3 weight)
  {
    params.N = ensure_valid_reflection(sd->Ng, sd->I, params.N);

    MicrofacetBsdf *bsdf = alloc(sd, path_flag, weight);
    if (!bsdf) {
      return;
    }

    bsdf->T = zero_float3();
    bsdf->alpha_y = bsdf->alpha_x;
    sd->flag |= bsdf_microfacet_multi_ggx_glass_fresnel_setup(bsdf, sd);
  }
};

ClosureParam *closure_bsdf_microfacet_multi_ggx_glass_fresnel_params()
{
  static ClosureParam params[] = {
      CLOSURE_FLOAT3_PARAM(MicrofacetMultiGGXFresnelClosure, params.N),
      CLOSURE_FLOAT_PARAM(MicrofacetMultiGGXFresnelClosure, params.alpha_x),
      CLOSURE_FLOAT_PARAM(MicrofacetMultiGGXFresnelClosure, params.ior),
      CLOSURE_FLOAT3_PARAM(MicrofacetMultiGGXFresnelClosure, color),
      CLOSURE_FLOAT3_PARAM(MicrofacetMultiGGXFresnelClosure, cspec0),
      CLOSURE_STRING_KEYPARAM(MicrofacetMultiGGXFresnelClosure, label, "label"),
      CLOSURE_FINISH_PARAM(MicrofacetMultiGGXFresnelClosure)};
  return params;
}
CCLOSURE_PREPARE(closure_bsdf_microfacet_multi_ggx_glass_fresnel_prepare,
                 MicrofacetMultiGGXGlassFresnelClosure);

/* Transparent */

class TransparentClosure : public CBSDFClosure {
 public:
  ShaderClosure params;
  float3 unused;

  void setup(ShaderData *sd, uint32_t path_flag, float3 weight)
  {
    bsdf_transparent_setup(sd, rgb_to_spectrum(weight), path_flag);
  }
};

ClosureParam *closure_bsdf_transparent_params()
{
  static ClosureParam params[] = {CLOSURE_STRING_KEYPARAM(TransparentClosure, label, "label"),
                                  CLOSURE_FINISH_PARAM(TransparentClosure)};
  return params;
}

CCLOSURE_PREPARE(closure_bsdf_transparent_prepare, TransparentClosure)

/* Volume */

class VolumeAbsorptionClosure : public CBSDFClosure {
 public:
  void setup(ShaderData *sd, uint32_t path_flag, float3 weight)
  {
    volume_extinction_setup(sd, rgb_to_spectrum(weight));
  }
};

ClosureParam *closure_absorption_params()
{
  static ClosureParam params[] = {CLOSURE_STRING_KEYPARAM(VolumeAbsorptionClosure, label, "label"),
                                  CLOSURE_FINISH_PARAM(VolumeAbsorptionClosure)};
  return params;
}

CCLOSURE_PREPARE(closure_absorption_prepare, VolumeAbsorptionClosure)

class VolumeHenyeyGreensteinClosure : public CBSDFClosure {
 public:
  HenyeyGreensteinVolume params;

  void setup(ShaderData *sd, uint32_t path_flag, float3 weight)
  {
    volume_extinction_setup(sd, rgb_to_spectrum(weight));

    HenyeyGreensteinVolume *volume = (HenyeyGreensteinVolume *)bsdf_alloc_osl(
        sd, sizeof(HenyeyGreensteinVolume), rgb_to_spectrum(weight), &params);
    if (!volume) {
      return;
    }

    sd->flag |= volume_henyey_greenstein_setup(volume);
  }
};

ClosureParam *closure_henyey_greenstein_params()
{
  static ClosureParam params[] = {
      CLOSURE_FLOAT_PARAM(VolumeHenyeyGreensteinClosure, params.g),
      CLOSURE_STRING_KEYPARAM(VolumeHenyeyGreensteinClosure, label, "label"),
      CLOSURE_FINISH_PARAM(VolumeHenyeyGreensteinClosure)};
  return params;
}

CCLOSURE_PREPARE(closure_henyey_greenstein_prepare, VolumeHenyeyGreensteinClosure)

CCL_NAMESPACE_END
