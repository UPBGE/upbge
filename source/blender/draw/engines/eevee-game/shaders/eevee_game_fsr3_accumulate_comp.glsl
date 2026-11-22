/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * FSR 3.1 upscaler: reprojection, upsampling and accumulation at output resolution.
 * Entry point of FidelityFX's ffx_fsr3upscaler_accumulate_pass.glsl (SDK v1.1.4).
 */

#include "infos/eevee_game_fsr3_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_game_fsr3_accumulate_base)

#include "eevee_game_fsr3_callbacks_lib.glsl"
#include "ffx_core_lib.glsl"
#include "ffx_fsr3upscaler_common_lib.glsl"
#include "ffx_fsr3upscaler_sample_lib.glsl"
#include "ffx_fsr3upscaler_upsample_lib.glsl"
#include "ffx_fsr3upscaler_reproject_lib.glsl"
#include "ffx_fsr3upscaler_accumulate_lib.glsl"

void main()
{
  const int2 iPxPos = int2(gl_GlobalInvocationID.xy);
  /* FidelityFX relies on out of bounds stores being dropped, Metal does not. */
  if (any(greaterThanEqual(iPxPos, UpscaleSize()))) {
    return;
  }
  Accumulate(iPxPos);
}
