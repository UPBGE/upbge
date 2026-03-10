/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/compositor_realize_on_domain_infos.hh"

COMPUTE_SHADER_CREATE_INFO(compositor_realize_on_domain_bicubic_float)

#include "gpu_shader_bicubic_sampler_lib.glsl"
#include "gpu_shader_compositor_texture_utilities.glsl"
#include "gpu_shader_math_matrix_transform_lib.glsl"

void main()
{
  const int2 texel = int2(gl_GlobalInvocationID.xy);
  const float2 coordinates = transform_point(to_float3x3(transformation), float2(texel));
  imageStore(domain_img, texel, SAMPLER_FUNCTION(input_tx, coordinates));
}
