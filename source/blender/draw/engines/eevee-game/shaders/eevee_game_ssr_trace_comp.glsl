/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Screen-space reflections: trace (half resolution).
 *
 * For the first GGX reflection closure of a pixel, smooth enough to be worth it, traces the
 * mirror direction (eevee_game_ssr_lib.glsl) against the previous frame's opaque radiance,
 * reprojected. The lighting pass blends the world reflection toward the result. Rough surfaces
 * fade out: one mirror ray does not stand for a wide lobe.
 *
 * Local group 8x8.
 */

#include "infos/eevee_game_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_game_ssr_trace)

#include "eevee_gbuffer_read_lib.glsl"
#include "eevee_game_ssr_lib.glsl"
#include "eevee_reverse_z_lib.glsl"

float ign(float2 px)
{
  return fract(52.9829189f * fract(dot(px, float2(0.06711056f, 0.00583715f))));
}

void main()
{
  const int2 out_px = int2(gl_GlobalInvocationID.xy);
  const int2 out_res = imageSize(out_ssr_img);
  if (any(greaterThanEqual(out_px, out_res))) {
    return;
  }

  const int2 texel = min(out_px * 2, int2(screen_res) - 1);
  float depth = reverse_z::read(texelFetch(depth_tx, texel, 0).r);
  if (depth >= 1.0f) {
    imageStore(out_ssr_img, out_px, float4(0.0f));
    return;
  }
  const gbuffer::Layers gbuf = gbuffer::read_layers(texel);
  /* First reflection closure: the one the lighting pass blends. */
  float roughness = 2.0f;
  float3 N = float3(0.0f, 0.0f, 1.0f);
  for (int i = 0; i < GBUFFER_LAYER_MAX; i++) {
    ClosureUndetermined cl = gbuf.layer_get(uchar(i));
    if (cl.type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID) {
      roughness = to_closure_reflection(cl).roughness;
      N = cl.N;
      break;
    }
  }

  const float2 uv = (float2(texel) + 0.5f) / screen_res;
  const float3 P = ssr_screen_to_world(viewprojinv, uv, depth);
  float4 result = ssr_trace(P,
                            N,
                            roughness,
                            ign(float2(out_px)),
                            camera_pos,
                            viewproj,
                            viewprojinv,
                            viewproj_prev,
                            z_planes,
                            screen_res,
                            hiz_uv_scale,
                            ssr_max_steps,
                            ssr_thickness,
                            ssr_max_roughness);
  imageStore(out_ssr_img, out_px, result);
}
