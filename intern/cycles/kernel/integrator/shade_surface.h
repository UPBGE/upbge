/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#pragma once

#include "kernel/film/accumulate.h"
#include "kernel/film/passes.h"

#include "kernel/integrator/mnee.h"

#include "kernel/integrator/path_state.h"
#include "kernel/integrator/shader_eval.h"
#include "kernel/integrator/subsurface.h"
#include "kernel/integrator/volume_stack.h"

#include "kernel/light/light.h"
#include "kernel/light/sample.h"

CCL_NAMESPACE_BEGIN

ccl_device_forceinline void integrate_surface_shader_setup(KernelGlobals kg,
                                                           ConstIntegratorState state,
                                                           ccl_private ShaderData *sd)
{
  Intersection isect ccl_optional_struct_init;
  integrator_state_read_isect(kg, state, &isect);

  Ray ray ccl_optional_struct_init;
  integrator_state_read_ray(kg, state, &ray);

  shader_setup_from_ray(kg, sd, &ray, &isect);
}

ccl_device_forceinline float3 integrate_surface_ray_offset(KernelGlobals kg,
                                                           const ccl_private ShaderData *sd,
                                                           const float3 ray_P,
                                                           const float3 ray_D)
{
  /* No ray offset needed for other primitive types. */
  if (!(sd->type & PRIMITIVE_TRIANGLE)) {
    return ray_P;
  }

  /* Self intersection tests already account for the case where a ray hits the
   * same primitive. However precision issues can still cause neighboring
   * triangles to be hit. Here we test if the ray-triangle intersection with
   * the same primitive would miss, implying that a neighboring triangle would
   * be hit instead.
   *
   * This relies on triangle intersection to be watertight, and the object inverse
   * object transform to match the one used by ray intersection exactly.
   *
   * Potential improvements:
   * - It appears this happens when either barycentric coordinates are small,
   *   or dot(sd->Ng, ray_D)  is small. Detect such cases and skip test?
   * - Instead of ray offset, can we tweak P to lie within the triangle?
   */
  const uint tri_vindex = kernel_data_fetch(tri_vindex, sd->prim).w;
  const packed_float3 tri_a = kernel_data_fetch(tri_verts, tri_vindex + 0),
                      tri_b = kernel_data_fetch(tri_verts, tri_vindex + 1),
                      tri_c = kernel_data_fetch(tri_verts, tri_vindex + 2);

  float3 local_ray_P = ray_P;
  float3 local_ray_D = ray_D;

  if (!(sd->object_flag & SD_OBJECT_TRANSFORM_APPLIED)) {
    const Transform itfm = object_get_inverse_transform(kg, sd);
    local_ray_P = transform_point(&itfm, local_ray_P);
    local_ray_D = transform_direction(&itfm, local_ray_D);
  }

  if (ray_triangle_intersect_self(local_ray_P, local_ray_D, tri_a, tri_b, tri_c)) {
    return ray_P;
  }
  else {
    return ray_offset(ray_P, sd->Ng);
  }
}

#ifdef __HOLDOUT__
ccl_device_forceinline bool integrate_surface_holdout(KernelGlobals kg,
                                                      ConstIntegratorState state,
                                                      ccl_private ShaderData *sd,
                                                      ccl_global float *ccl_restrict render_buffer)
{
  /* Write holdout transparency to render buffer and stop if fully holdout. */
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

  if (((sd->flag & SD_HOLDOUT) || (sd->object_flag & SD_OBJECT_HOLDOUT_MASK)) &&
      (path_flag & PATH_RAY_TRANSPARENT_BACKGROUND)) {
    const Spectrum holdout_weight = shader_holdout_apply(kg, sd);
    const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput);
    const float transparent = average(holdout_weight * throughput);
    kernel_accum_holdout(kg, state, path_flag, transparent, render_buffer);
    if (isequal(holdout_weight, one_spectrum())) {
      return false;
    }
  }

  return true;
}
#endif /* __HOLDOUT__ */

#ifdef __EMISSION__
ccl_device_forceinline void integrate_surface_emission(KernelGlobals kg,
                                                       ConstIntegratorState state,
                                                       ccl_private const ShaderData *sd,
                                                       ccl_global float *ccl_restrict
                                                           render_buffer)
{
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

  /* Evaluate emissive closure. */
  Spectrum L = shader_emissive_eval(sd);

#  ifdef __HAIR__
  if (!(path_flag & PATH_RAY_MIS_SKIP) && (sd->flag & SD_USE_MIS) &&
      (sd->type & PRIMITIVE_TRIANGLE))
#  else
  if (!(path_flag & PATH_RAY_MIS_SKIP) && (sd->flag & SD_USE_MIS))
#  endif
  {
    const float bsdf_pdf = INTEGRATOR_STATE(state, path, mis_ray_pdf);
    const float t = sd->ray_length;

    /* Multiple importance sampling, get triangle light pdf,
     * and compute weight with respect to BSDF pdf. */
    float pdf = triangle_light_pdf(kg, sd, t);
    float mis_weight = light_sample_mis_weight_forward(kg, bsdf_pdf, pdf);
    L *= mis_weight;
  }

  const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput);
  kernel_accum_emission(
      kg, state, throughput * L, render_buffer, object_lightgroup(kg, sd->object));
}
#endif /* __EMISSION__ */

#ifdef __EMISSION__
/* Path tracing: sample point on light and evaluate light shader, then
 * queue shadow ray to be traced. */
template<uint node_feature_mask>
ccl_device_forceinline void integrate_surface_direct_light(KernelGlobals kg,
                                                           IntegratorState state,
                                                           ccl_private ShaderData *sd,
                                                           ccl_private const RNGState *rng_state)
{
  /* Test if there is a light or BSDF that needs direct light. */
  if (!(kernel_data.integrator.use_direct_light && (sd->flag & SD_BSDF_HAS_EVAL))) {
    return;
  }

  /* Sample position on a light. */
  LightSample ls ccl_optional_struct_init;
  {
    const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
    const uint bounce = INTEGRATOR_STATE(state, path, bounce);
    float light_u, light_v;
    path_state_rng_2D(kg, rng_state, PRNG_LIGHT_U, &light_u, &light_v);

    if (!light_distribution_sample_from_position(
            kg, light_u, light_v, sd->time, sd->P, bounce, path_flag, &ls)) {
      return;
    }
  }

  kernel_assert(ls.pdf != 0.0f);

  /* Evaluate light shader.
   *
   * TODO: can we reuse sd memory? In theory we can move this after
   * integrate_surface_bounce, evaluate the BSDF, and only then evaluate
   * the light shader. This could also move to its own kernel, for
   * non-constant light sources. */
  ShaderDataCausticsStorage emission_sd_storage;
  ccl_private ShaderData *emission_sd = AS_SHADER_DATA(&emission_sd_storage);

  Ray ray ccl_optional_struct_init;
  BsdfEval bsdf_eval ccl_optional_struct_init;
  const bool is_transmission = shader_bsdf_is_transmission(sd, ls.D);

#  ifdef __MNEE__
  int mnee_vertex_count = 0;
  IF_KERNEL_FEATURE(MNEE)
  {
    if (ls.lamp != LAMP_NONE) {
      /* Is this a caustic light? */
      const bool use_caustics = kernel_data_fetch(lights, ls.lamp).use_caustics;
      if (use_caustics) {
        /* Are we on a caustic caster? */
        if (is_transmission && (sd->object_flag & SD_OBJECT_CAUSTICS_CASTER))
          return;

        /* Are we on a caustic receiver? */
        if (!is_transmission && (sd->object_flag & SD_OBJECT_CAUSTICS_RECEIVER))
          mnee_vertex_count = kernel_path_mnee_sample(
              kg, state, sd, emission_sd, rng_state, &ls, &bsdf_eval);
      }
    }
  }
  if (mnee_vertex_count > 0) {
    /* Create shadow ray after successful manifold walk:
     * emission_sd contains the last interface intersection and
     * the light sample ls has been updated */
    light_sample_to_surface_shadow_ray(kg, emission_sd, &ls, &ray);
  }
  else
#  endif /* __MNEE__ */
  {
    const Spectrum light_eval = light_sample_shader_eval(kg, state, emission_sd, &ls, sd->time);
    if (is_zero(light_eval)) {
      return;
    }

    /* Evaluate BSDF. */
    const float bsdf_pdf = shader_bsdf_eval(kg, sd, ls.D, is_transmission, &bsdf_eval, ls.shader);
    bsdf_eval_mul(&bsdf_eval, light_eval / ls.pdf);

    if (ls.shader & SHADER_USE_MIS) {
      const float mis_weight = light_sample_mis_weight_nee(kg, ls.pdf, bsdf_pdf);
      bsdf_eval_mul(&bsdf_eval, mis_weight);
    }

    /* Path termination. */
    const float terminate = path_state_rng_light_termination(kg, rng_state);
    if (light_sample_terminate(kg, &ls, &bsdf_eval, terminate)) {
      return;
    }

    /* Create shadow ray. */
    light_sample_to_surface_shadow_ray(kg, sd, &ls, &ray);
  }

  const bool is_light = light_sample_is_light(&ls);

  /* Branch off shadow kernel. */
  IntegratorShadowState shadow_state = integrator_shadow_path_init(
      kg, state, DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW, false);

  /* Copy volume stack and enter/exit volume. */
  integrator_state_copy_volume_stack_to_shadow(kg, shadow_state, state);

  if (is_transmission) {
#  ifdef __VOLUME__
    shadow_volume_stack_enter_exit(kg, shadow_state, sd);
#  endif
  }

  if (ray.self.object != OBJECT_NONE) {
    ray.P = integrate_surface_ray_offset(kg, sd, ray.P, ray.D);
  }

  /* Write shadow ray and associated state to global memory. */
  integrator_state_write_shadow_ray(kg, shadow_state, &ray);
  // Save memory by storing the light and object indices in the shadow_isect
  INTEGRATOR_STATE_ARRAY_WRITE(shadow_state, shadow_isect, 0, object) = ray.self.object;
  INTEGRATOR_STATE_ARRAY_WRITE(shadow_state, shadow_isect, 0, prim) = ray.self.prim;
  INTEGRATOR_STATE_ARRAY_WRITE(shadow_state, shadow_isect, 1, object) = ray.self.light_object;
  INTEGRATOR_STATE_ARRAY_WRITE(shadow_state, shadow_isect, 1, prim) = ray.self.light_prim;

  /* Copy state from main path to shadow path. */
  uint32_t shadow_flag = INTEGRATOR_STATE(state, path, flag);
  shadow_flag |= (is_light) ? PATH_RAY_SHADOW_FOR_LIGHT : 0;
  const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput) *
                              bsdf_eval_sum(&bsdf_eval);

  if (kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_PASSES) {
    PackedSpectrum pass_diffuse_weight;
    PackedSpectrum pass_glossy_weight;

    if (shadow_flag & PATH_RAY_ANY_PASS) {
      /* Indirect bounce, use weights from earlier surface or volume bounce. */
      pass_diffuse_weight = INTEGRATOR_STATE(state, path, pass_diffuse_weight);
      pass_glossy_weight = INTEGRATOR_STATE(state, path, pass_glossy_weight);
    }
    else {
      /* Direct light, use BSDFs at this bounce. */
      shadow_flag |= PATH_RAY_SURFACE_PASS;
      pass_diffuse_weight = PackedSpectrum(bsdf_eval_pass_diffuse_weight(&bsdf_eval));
      pass_glossy_weight = PackedSpectrum(bsdf_eval_pass_glossy_weight(&bsdf_eval));
    }

    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, pass_diffuse_weight) = pass_diffuse_weight;
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, pass_glossy_weight) = pass_glossy_weight;
  }

  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, render_pixel_index) = INTEGRATOR_STATE(
      state, path, render_pixel_index);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, rng_offset) = INTEGRATOR_STATE(
      state, path, rng_offset);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, rng_hash) = INTEGRATOR_STATE(
      state, path, rng_hash);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, sample) = INTEGRATOR_STATE(
      state, path, sample);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, flag) = shadow_flag;

  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, transparent_bounce) = INTEGRATOR_STATE(
      state, path, transparent_bounce);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, glossy_bounce) = INTEGRATOR_STATE(
      state, path, glossy_bounce);

#  ifdef __MNEE__
  if (mnee_vertex_count > 0) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, transmission_bounce) =
        INTEGRATOR_STATE(state, path, transmission_bounce) + mnee_vertex_count - 1;
    INTEGRATOR_STATE_WRITE(shadow_state,
                           shadow_path,
                           diffuse_bounce) = INTEGRATOR_STATE(state, path, diffuse_bounce) + 1;
    INTEGRATOR_STATE_WRITE(shadow_state,
                           shadow_path,
                           bounce) = INTEGRATOR_STATE(state, path, bounce) + mnee_vertex_count;
  }
  else
#  endif
  {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, transmission_bounce) = INTEGRATOR_STATE(
        state, path, transmission_bounce);
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, diffuse_bounce) = INTEGRATOR_STATE(
        state, path, diffuse_bounce);
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, bounce) = INTEGRATOR_STATE(
        state, path, bounce);
  }

  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, throughput) = throughput;

  if (kernel_data.kernel_features & KERNEL_FEATURE_SHADOW_PASS) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, unshadowed_throughput) = throughput;
  }

  /* Write Lightgroup, +1 as lightgroup is int but we need to encode into a uint8_t. */
  INTEGRATOR_STATE_WRITE(
      shadow_state, shadow_path, lightgroup) = (ls.type != LIGHT_BACKGROUND) ?
                                                   ls.group + 1 :
                                                   kernel_data.background.lightgroup + 1;
}
#endif

/* Path tracing: bounce off or through surface with new direction. */
ccl_device_forceinline int integrate_surface_bsdf_bssrdf_bounce(
    KernelGlobals kg,
    IntegratorState state,
    ccl_private ShaderData *sd,
    ccl_private const RNGState *rng_state)
{
  /* Sample BSDF or BSSRDF. */
  if (!(sd->flag & (SD_BSDF | SD_BSSRDF))) {
    return LABEL_NONE;
  }

  float bsdf_u, bsdf_v;
  path_state_rng_2D(kg, rng_state, PRNG_BSDF_U, &bsdf_u, &bsdf_v);
  ccl_private const ShaderClosure *sc = shader_bsdf_bssrdf_pick(sd, &bsdf_u);

#ifdef __SUBSURFACE__
  /* BSSRDF closure, we schedule subsurface intersection kernel. */
  if (CLOSURE_IS_BSSRDF(sc->type)) {
    return subsurface_bounce(kg, state, sd, sc);
  }
#endif

  /* BSDF closure, sample direction. */
  float bsdf_pdf;
  BsdfEval bsdf_eval ccl_optional_struct_init;
  float3 bsdf_omega_in ccl_optional_struct_init;
  int label;

  label = shader_bsdf_sample_closure(
      kg, sd, sc, bsdf_u, bsdf_v, &bsdf_eval, &bsdf_omega_in, &bsdf_pdf);

  if (bsdf_pdf == 0.0f || bsdf_eval_is_zero(&bsdf_eval)) {
    return LABEL_NONE;
  }

  if (label & LABEL_TRANSPARENT) {
    /* Only need to modify start distance for transparent. */
    INTEGRATOR_STATE_WRITE(state, ray, tmin) = intersection_t_offset(sd->ray_length);
  }
  else {
    /* Setup ray with changed origin and direction. */
    const float3 D = normalize(bsdf_omega_in);
    INTEGRATOR_STATE_WRITE(state, ray, P) = integrate_surface_ray_offset(kg, sd, sd->P, D);
    INTEGRATOR_STATE_WRITE(state, ray, D) = D;
    INTEGRATOR_STATE_WRITE(state, ray, tmin) = 0.0f;
    INTEGRATOR_STATE_WRITE(state, ray, tmax) = FLT_MAX;
#ifdef __RAY_DIFFERENTIALS__
    INTEGRATOR_STATE_WRITE(state, ray, dP) = differential_make_compact(sd->dP);
#endif
  }

  /* Update throughput. */
  Spectrum throughput = INTEGRATOR_STATE(state, path, throughput);
  throughput *= bsdf_eval_sum(&bsdf_eval) / bsdf_pdf;
  INTEGRATOR_STATE_WRITE(state, path, throughput) = throughput;

  if (kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_PASSES) {
    if (INTEGRATOR_STATE(state, path, bounce) == 0) {
      INTEGRATOR_STATE_WRITE(state, path, pass_diffuse_weight) = bsdf_eval_pass_diffuse_weight(
          &bsdf_eval);
      INTEGRATOR_STATE_WRITE(state, path, pass_glossy_weight) = bsdf_eval_pass_glossy_weight(
          &bsdf_eval);
    }
  }

  /* Update path state */
  if (!(label & LABEL_TRANSPARENT)) {
    INTEGRATOR_STATE_WRITE(state, path, mis_ray_pdf) = bsdf_pdf;
    INTEGRATOR_STATE_WRITE(state, path, min_ray_pdf) = fminf(
        bsdf_pdf, INTEGRATOR_STATE(state, path, min_ray_pdf));
  }

  path_state_next(kg, state, label);
  return label;
}

#ifdef __VOLUME__
ccl_device_forceinline int integrate_surface_volume_only_bounce(IntegratorState state,
                                                                ccl_private ShaderData *sd)
{
  if (!path_state_volume_next(state)) {
    return LABEL_NONE;
  }

  /* Only modify start distance. */
  INTEGRATOR_STATE_WRITE(state, ray, tmin) = intersection_t_offset(sd->ray_length);

  return LABEL_TRANSMIT | LABEL_TRANSPARENT;
}
#endif

ccl_device_forceinline bool integrate_surface_terminate(IntegratorState state,
                                                        const uint32_t path_flag)
{
  const float probability = (path_flag & PATH_RAY_TERMINATE_ON_NEXT_SURFACE) ?
                                0.0f :
                                INTEGRATOR_STATE(state, path, continuation_probability);
  if (probability == 0.0f) {
    return true;
  }
  else if (probability != 1.0f) {
    INTEGRATOR_STATE_WRITE(state, path, throughput) /= probability;
  }

  return false;
}

#if defined(__AO__)
ccl_device_forceinline void integrate_surface_ao(KernelGlobals kg,
                                                 IntegratorState state,
                                                 ccl_private const ShaderData *ccl_restrict sd,
                                                 ccl_private const RNGState *ccl_restrict
                                                     rng_state,
                                                 ccl_global float *ccl_restrict render_buffer)
{
  if (!(kernel_data.kernel_features & KERNEL_FEATURE_AO_ADDITIVE) &&
      !(INTEGRATOR_STATE(state, path, flag) & PATH_RAY_CAMERA)) {
    return;
  }

  float bsdf_u, bsdf_v;
  path_state_rng_2D(kg, rng_state, PRNG_BSDF_U, &bsdf_u, &bsdf_v);

  float3 ao_N;
  const Spectrum ao_weight = shader_bsdf_ao(
      kg, sd, kernel_data.integrator.ao_additive_factor, &ao_N);

  float3 ao_D;
  float ao_pdf;
  sample_cos_hemisphere(ao_N, bsdf_u, bsdf_v, &ao_D, &ao_pdf);

  bool skip_self = true;

  Ray ray ccl_optional_struct_init;
  ray.P = shadow_ray_offset(kg, sd, ao_D, &skip_self);
  ray.D = ao_D;
  if (skip_self) {
    ray.P = integrate_surface_ray_offset(kg, sd, ray.P, ray.D);
  }
  ray.tmin = 0.0f;
  ray.tmax = kernel_data.integrator.ao_bounces_distance;
  ray.time = sd->time;
  ray.self.object = (skip_self) ? sd->object : OBJECT_NONE;
  ray.self.prim = (skip_self) ? sd->prim : PRIM_NONE;
  ray.self.light_object = OBJECT_NONE;
  ray.self.light_prim = PRIM_NONE;
  ray.dP = differential_zero_compact();
  ray.dD = differential_zero_compact();

  /* Branch off shadow kernel. */
  IntegratorShadowState shadow_state = integrator_shadow_path_init(
      kg, state, DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW, true);

  /* Copy volume stack and enter/exit volume. */
  integrator_state_copy_volume_stack_to_shadow(kg, shadow_state, state);

  /* Write shadow ray and associated state to global memory. */
  integrator_state_write_shadow_ray(kg, shadow_state, &ray);
  INTEGRATOR_STATE_ARRAY_WRITE(shadow_state, shadow_isect, 0, object) = ray.self.object;
  INTEGRATOR_STATE_ARRAY_WRITE(shadow_state, shadow_isect, 0, prim) = ray.self.prim;
  INTEGRATOR_STATE_ARRAY_WRITE(shadow_state, shadow_isect, 1, object) = ray.self.light_object;
  INTEGRATOR_STATE_ARRAY_WRITE(shadow_state, shadow_isect, 1, prim) = ray.self.light_prim;

  /* Copy state from main path to shadow path. */
  const uint16_t bounce = INTEGRATOR_STATE(state, path, bounce);
  const uint16_t transparent_bounce = INTEGRATOR_STATE(state, path, transparent_bounce);
  uint32_t shadow_flag = INTEGRATOR_STATE(state, path, flag) | PATH_RAY_SHADOW_FOR_AO;
  const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput) *
                              shader_bsdf_alpha(kg, sd);

  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, render_pixel_index) = INTEGRATOR_STATE(
      state, path, render_pixel_index);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, rng_offset) = INTEGRATOR_STATE(
      state, path, rng_offset);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, rng_hash) = INTEGRATOR_STATE(
      state, path, rng_hash);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, sample) = INTEGRATOR_STATE(
      state, path, sample);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, flag) = shadow_flag;
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, bounce) = bounce;
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, transparent_bounce) = transparent_bounce;
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, throughput) = throughput;

  if (kernel_data.kernel_features & KERNEL_FEATURE_AO_ADDITIVE) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, unshadowed_throughput) = ao_weight;
  }
}
#endif /* defined(__AO__) */

template<uint node_feature_mask>
ccl_device bool integrate_surface(KernelGlobals kg,
                                  IntegratorState state,
                                  ccl_global float *ccl_restrict render_buffer)

{
  PROFILING_INIT_FOR_SHADER(kg, PROFILING_SHADE_SURFACE_SETUP);

  /* Setup shader data. */
  ShaderData sd;
  integrate_surface_shader_setup(kg, state, &sd);
  PROFILING_SHADER(sd.object, sd.shader);

  int continue_path_label = 0;

  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

  /* Skip most work for volume bounding surface. */
#ifdef __VOLUME__
  if (!(sd.flag & SD_HAS_ONLY_VOLUME)) {
#endif
#ifdef __SUBSURFACE__
    /* Can skip shader evaluation for BSSRDF exit point without bump mapping. */
    if (!(path_flag & PATH_RAY_SUBSURFACE) || ((sd.flag & SD_HAS_BSSRDF_BUMP)))
#endif
    {
      /* Evaluate shader. */
      PROFILING_EVENT(PROFILING_SHADE_SURFACE_EVAL);
      shader_eval_surface<node_feature_mask>(kg, state, &sd, render_buffer, path_flag);

      /* Initialize additional RNG for BSDFs. */
      if (sd.flag & SD_BSDF_NEEDS_LCG) {
        sd.lcg_state = lcg_state_init(INTEGRATOR_STATE(state, path, rng_hash),
                                      INTEGRATOR_STATE(state, path, rng_offset),
                                      INTEGRATOR_STATE(state, path, sample),
                                      0xb4bc3953);
      }
    }

#ifdef __SUBSURFACE__
    if (path_flag & PATH_RAY_SUBSURFACE) {
      /* When coming from inside subsurface scattering, setup a diffuse
       * closure to perform lighting at the exit point. */
      subsurface_shader_data_setup(kg, state, &sd, path_flag);
      INTEGRATOR_STATE_WRITE(state, path, flag) &= ~PATH_RAY_SUBSURFACE;
    }
    else
#endif
    {
      /* Filter closures. */
      shader_prepare_surface_closures(kg, state, &sd, path_flag);

#ifdef __HOLDOUT__
      /* Evaluate holdout. */
      if (!integrate_surface_holdout(kg, state, &sd, render_buffer)) {
        return false;
      }
#endif

#ifdef __EMISSION__
      /* Write emission. */
      if (sd.flag & SD_EMISSION) {
        integrate_surface_emission(kg, state, &sd, render_buffer);
      }
#endif

      /* Perform path termination. Most paths have already been terminated in
       * the intersect_closest kernel, this is just for emission and for dividing
       * throughput by the probability at the right moment.
       *
       * Also ensure we don't do it twice for SSS at both the entry and exit point. */
      if (integrate_surface_terminate(state, path_flag)) {
        return false;
      }

      /* Write render passes. */
#ifdef __PASSES__
      PROFILING_EVENT(PROFILING_SHADE_SURFACE_PASSES);
      kernel_write_data_passes(kg, state, &sd, render_buffer);
#endif

#ifdef __DENOISING_FEATURES__
      kernel_write_denoising_features_surface(kg, state, &sd, render_buffer);
#endif
    }

    /* Load random number state. */
    RNGState rng_state;
    path_state_rng_load(state, &rng_state);

    /* Direct light. */
    PROFILING_EVENT(PROFILING_SHADE_SURFACE_DIRECT_LIGHT);
    integrate_surface_direct_light<node_feature_mask>(kg, state, &sd, &rng_state);

#if defined(__AO__)
    /* Ambient occlusion pass. */
    if (kernel_data.kernel_features & KERNEL_FEATURE_AO) {
      PROFILING_EVENT(PROFILING_SHADE_SURFACE_AO);
      integrate_surface_ao(kg, state, &sd, &rng_state, render_buffer);
    }
#endif

    PROFILING_EVENT(PROFILING_SHADE_SURFACE_INDIRECT_LIGHT);
    continue_path_label = integrate_surface_bsdf_bssrdf_bounce(kg, state, &sd, &rng_state);
#ifdef __VOLUME__
  }
  else {
    if (integrate_surface_terminate(state, path_flag)) {
      return false;
    }

    PROFILING_EVENT(PROFILING_SHADE_SURFACE_INDIRECT_LIGHT);
    continue_path_label = integrate_surface_volume_only_bounce(state, &sd);
  }

  if (continue_path_label & LABEL_TRANSMIT) {
    /* Enter/Exit volume. */
    volume_stack_enter_exit(kg, state, &sd);
  }
#endif

  return continue_path_label != 0;
}

template<uint node_feature_mask = KERNEL_FEATURE_NODE_MASK_SURFACE & ~KERNEL_FEATURE_NODE_RAYTRACE,
         DeviceKernel current_kernel = DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE>
ccl_device_forceinline void integrator_shade_surface(KernelGlobals kg,
                                                     IntegratorState state,
                                                     ccl_global float *ccl_restrict render_buffer)
{
  if (integrate_surface<node_feature_mask>(kg, state, render_buffer)) {
    if (INTEGRATOR_STATE(state, path, flag) & PATH_RAY_SUBSURFACE) {
      integrator_path_next(
          kg, state, current_kernel, DEVICE_KERNEL_INTEGRATOR_INTERSECT_SUBSURFACE);
    }
    else {
      kernel_assert(INTEGRATOR_STATE(state, ray, tmax) != 0.0f);
      integrator_path_next(kg, state, current_kernel, DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST);
    }
  }
  else {
    integrator_path_terminate(kg, state, current_kernel);
  }
}

ccl_device_forceinline void integrator_shade_surface_raytrace(
    KernelGlobals kg, IntegratorState state, ccl_global float *ccl_restrict render_buffer)
{
  integrator_shade_surface<KERNEL_FEATURE_NODE_MASK_SURFACE,
                           DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_RAYTRACE>(
      kg, state, render_buffer);
}

ccl_device_forceinline void integrator_shade_surface_mnee(
    KernelGlobals kg, IntegratorState state, ccl_global float *ccl_restrict render_buffer)
{
  integrator_shade_surface<(KERNEL_FEATURE_NODE_MASK_SURFACE & ~KERNEL_FEATURE_NODE_RAYTRACE) |
                               KERNEL_FEATURE_MNEE,
                           DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_MNEE>(kg, state, render_buffer);
}

CCL_NAMESPACE_END
