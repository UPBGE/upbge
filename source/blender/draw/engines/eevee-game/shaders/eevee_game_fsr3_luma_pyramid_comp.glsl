/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * FSR 3.1 upscaler: SPD over luma and farthest depth, frame info.
 * Entry point of FidelityFX's ffx_fsr3upscaler_luma_pyramid_pass.glsl (SDK v1.1.4).
 */

#include "infos/eevee_game_fsr3_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_game_fsr3_luma_pyramid)

#include "eevee_game_fsr3_callbacks_lib.glsl"
#include "ffx_core_lib.glsl"
#include "ffx_fsr3upscaler_common_lib.glsl"
#include "ffx_fsr3upscaler_luma_pyramid_lib.glsl"

void main()
{
  ComputeAutoExposure(gl_WorkGroupID, gl_LocalInvocationIndex);
}
