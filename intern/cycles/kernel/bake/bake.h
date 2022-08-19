/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#pragma once

#include "kernel/camera/projection.h"
#include "kernel/integrator/shader_eval.h"

#include "kernel/geom/geom.h"

#include "kernel/util/color.h"

CCL_NAMESPACE_BEGIN

ccl_device void kernel_displace_evaluate(KernelGlobals kg,
                                         ccl_global const KernelShaderEvalInput *input,
                                         ccl_global float *output,
                                         const int offset)
{
  /* Setup shader data. */
  const KernelShaderEvalInput in = input[offset];

  ShaderData sd;
  shader_setup_from_displace(kg, &sd, in.object, in.prim, in.u, in.v);

  /* Evaluate displacement shader. */
  const float3 P = sd.P;
  shader_eval_displacement(kg, INTEGRATOR_STATE_NULL, &sd);
  float3 D = sd.P - P;

  object_inverse_dir_transform(kg, &sd, &D);

#ifdef __KERNEL_DEBUG_NAN__
  if (!isfinite_safe(D)) {
    kernel_assert(!"Cycles displacement with non-finite value detected");
  }
#endif

  /* Ensure finite displacement, preventing BVH from becoming degenerate and avoiding possible
   * traversal issues caused by non-finite math. */
  D = ensure_finite(D);

  /* Write output. */
  output[offset * 3 + 0] += D.x;
  output[offset * 3 + 1] += D.y;
  output[offset * 3 + 2] += D.z;
}

ccl_device void kernel_background_evaluate(KernelGlobals kg,
                                           ccl_global const KernelShaderEvalInput *input,
                                           ccl_global float *output,
                                           const int offset)
{
  /* Setup ray */
  const KernelShaderEvalInput in = input[offset];
  const float3 ray_P = zero_float3();
  const float3 ray_D = equirectangular_to_direction(in.u, in.v);
  const float ray_time = 0.5f;

  /* Setup shader data. */
  ShaderData sd;
  shader_setup_from_background(kg, &sd, ray_P, ray_D, ray_time);

  /* Evaluate shader.
   * This is being evaluated for all BSDFs, so path flag does not contain a specific type. */
  const uint32_t path_flag = PATH_RAY_EMISSION;
  shader_eval_surface<KERNEL_FEATURE_NODE_MASK_SURFACE_LIGHT &
                      ~(KERNEL_FEATURE_NODE_RAYTRACE | KERNEL_FEATURE_NODE_LIGHT_PATH)>(
      kg, INTEGRATOR_STATE_NULL, &sd, NULL, path_flag);
  Spectrum color = shader_background_eval(&sd);

#ifdef __KERNEL_DEBUG_NAN__
  if (!isfinite_safe(color)) {
    kernel_assert(!"Cycles background with non-finite value detected");
  }
#endif

  /* Ensure finite color, avoiding possible numerical instabilities in the path tracing kernels. */
  color = ensure_finite(color);

  float3 color_rgb = spectrum_to_rgb(color);

  /* Write output. */
  output[offset * 3 + 0] += color_rgb.x;
  output[offset * 3 + 1] += color_rgb.y;
  output[offset * 3 + 2] += color_rgb.z;
}

ccl_device void kernel_curve_shadow_transparency_evaluate(
    KernelGlobals kg,
    ccl_global const KernelShaderEvalInput *input,
    ccl_global float *output,
    const int offset)
{
  /* Setup shader data. */
  const KernelShaderEvalInput in = input[offset];

  ShaderData sd;
  shader_setup_from_curve(kg, &sd, in.object, in.prim, __float_as_int(in.v), in.u);

  /* Evaluate transparency. */
  shader_eval_surface<KERNEL_FEATURE_NODE_MASK_SURFACE_SHADOW &
                      ~(KERNEL_FEATURE_NODE_RAYTRACE | KERNEL_FEATURE_NODE_LIGHT_PATH)>(
      kg, INTEGRATOR_STATE_NULL, &sd, NULL, PATH_RAY_SHADOW);

  /* Write output. */
  output[offset] = clamp(average(shader_bsdf_transparency(kg, &sd)), 0.0f, 1.0f);
}

CCL_NAMESPACE_END
