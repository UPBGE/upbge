/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/**
 * Game light evaluation shared by the deferred lighting pass and the forward
 * surface shaders: incoming light of one light, its shadow from the atlases,
 * and the GGX lobe. Closure dispatch stays in the callers, which have the
 * closure types from their own libraries.
 */

#include "infos/eevee_game_infos.hh"

SHADER_LIBRARY_CREATE_INFO(eevee_game_light_data)

#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_math_constants_lib.glsl"

/* GGX / Trowbridge-Reitz microfacet lobe times N.L, without Fresnel.
 *
 * EEVEE stores the specular color already multiplied by its Fresnel and
 * energy terms (the split-sum result is folded into the closure color when
 * the material is evaluated), so the lobe must not apply Fresnel again.
 * EEVEE additionally rescales its lobe to integrate to exactly one; this one
 * integrates to slightly less on rough surfaces, so they come out marginally
 * darker. Cheap, and not visible next to the other approximations here. */
float ggx_lobe(float3 N, float3 L, float3 V, float roughness)
{
  float NL = dot(N, L);
  if (NL <= 0.0f) {
    return 0.0f;
  }
  float NV = max(dot(N, V), 1e-4f);
  float3 H = normalize(L + V);
  float NH = saturate(dot(N, H));

  /* Perceptual roughness squared, floored: a perfect mirror has an
   * infinitely narrow lobe that a punctual light can never hit. */
  float a = max(roughness * roughness, 2e-3f);
  float a2 = a * a;

  /* D: fraction of microfacets whose normal is H. */
  float d = (NH * a2 - NH) * NH + 1.0f;
  float D = a2 / (M_PI * d * d);

  /* Height correlated Smith visibility, with the 1 / (4 NL NV) of the
   * microfacet BRDF already folded in. */
  float vis_v = NL * sqrt(NV * (NV - NV * a2) + a2);
  float vis_l = NV * sqrt(NL * (NL - NL * a2) + a2);
  float vis = 0.5f / max(vis_v + vis_l, 1e-5f);

  return D * vis * NL;
}

/* Light arriving at P from one light: direction towards the light and the
 * irradiance on a surface facing it. */
float3 light_incoming(LightData light, float3 P, out float3 L)
{
  if (light.type == LIGHT_TYPE_SUN) {
    /* Sun strength is already an irradiance (W/m2). */
    L = -light.direction;
    return light.color * light.energy;
  }

  float3 to_light = light.position - P;
  float dist_sqr = max(dot(to_light, to_light), 1e-4f);
  L = to_light * inversesqrt(dist_sqr);

  /* Blender's Watt is not radiometric: a point light of power P has a
   * radiant intensity of P / (4 pi^2), not P / (4 pi). */
  float intensity = light.energy * (0.25f * M_1_PI * M_1_PI);

  /* Inverse square falloff, windowed so it reaches exactly zero at the
   * light's range. A bounded light is what lets it be culled later. */
  float range_sqr = light.attenuation * light.attenuation;
  float window = saturate(1.0f - square(dist_sqr / max(range_sqr, 1e-4f)));
  float falloff = square(window) / dist_sqr;

  if (light.type == LIGHT_TYPE_SPOT) {
    /* Cone with a fixed soft edge over the outer 15% of the half angle. */
    float cos_outer = cos(light.spot_angle * 0.5f);
    float cos_inner = cos(light.spot_angle * 0.5f * 0.85f);
    float cos_theta = dot(-L, light.direction);
    falloff *= smoothstep(cos_outer, cos_inner, cos_theta);
  }
  /* Area lights are treated as point lights for now. */

  return light.color * (intensity * falloff);
}

/* Lit fraction of P in one shadow face: 3x3 PCF. dist is the distance to
 * the light for a perspective face (texel size grows with it), ignored for
 * an orthographic cascade. */
float shadow_tile_sample(int tile_index, float3 P, float3 N, float dist)
{
  ShadowTileData tile = shadow_tile_buf[tile_index];
  bool is_ortho = tile.view_proj[3][3] == 1.0f;

  /* Normal offset of 1.5 texels measured at the receiver. It removes acne
   * without a depth bias that would detach contact shadows. */
  float texel_world = tile.texel_scale * (is_ortho ? 1.0f : dist);
  float4 clip = tile.view_proj * float4(P + N * (1.5f * texel_world), 1.0f);
  if (clip.w <= 0.0f) {
    return 1.0f;
  }
  float3 ndc = clip.xyz / clip.w;
  if (any(greaterThan(abs(ndc.xy), float2(1.0f)))) {
    /* Outside the spot cone or the cascade: the cone falloff or the cascade
     * selection already handled it. */
    return 1.0f;
  }

  float2 atlas_size = (tile.atlas == 0) ? float2(textureSize(shadow_static_atlas, 0)) :
                                          float2(textureSize(shadow_dynamic_atlas, 0));
  float2 texel = 1.0f / atlas_size;
  float2 uv = tile.atlas_rect.xy + (ndc.xy * 0.5f + 0.5f) * tile.atlas_rect.zw;
  /* Taps stay inside the face, the guard band covers the footprint. */
  float2 lo = tile.atlas_rect.xy + 0.5f * texel;
  float2 hi = tile.atlas_rect.xy + tile.atlas_rect.zw - 0.5f * texel;
  /* The atlas holds reversed depth (1 = near the light). */
  float ref = 0.5f - 0.5f * ndc.z;

  /* `ref <= stored` (LEQUAL) holds where something at least as near as the
   * receiver was drawn: the occluded case. Explicit LOD: the atlas choice
   * makes the control flow non uniform. */
  float occluded = 0.0f;
  for (int y = -1; y <= 1; y++) {
    for (int x = -1; x <= 1; x++) {
      float3 tap = float3(clamp(uv + float2(x, y) * texel, lo, hi), ref);
      occluded += (tile.atlas == 0) ? textureLod(shadow_static_atlas, tap, 0.0f) :
                                      textureLod(shadow_dynamic_atlas, tap, 0.0f);
    }
  }
  return 1.0f - occluded * (1.0f / 9.0f);
}

/* Lit fraction of P for a shadowed light (shadow_index >= 0). view_depth is
 * the positive view space depth of P, used to pick the sun cascade. */
float game_light_shadow(LightData light, float3 P, float3 N, float view_depth)
{
  if (light.type == LIGHT_TYPE_SUN) {
    /* Four cascades, the first whose split covers the depth. */
    for (int c = 0; c < 4; c++) {
      int tile_index = light.shadow_index + c;
      if (view_depth < shadow_tile_buf[tile_index].split_depth) {
        return shadow_tile_sample(tile_index, P, N, 0.0f);
      }
    }
    /* Beyond the shadow distance. */
    return 1.0f;
  }

  float3 to_p = P - light.position;
  int tile_index = light.shadow_index;
  if (light.type == LIGHT_TYPE_POINT) {
    /* Faces are ordered +X -X +Y -Y +Z -Z. */
    float3 a = abs(to_p);
    int face;
    if (a.x >= a.y && a.x >= a.z) {
      face = (to_p.x > 0.0f) ? 0 : 1;
    }
    else if (a.y >= a.z) {
      face = (to_p.y > 0.0f) ? 2 : 3;
    }
    else {
      face = (to_p.z > 0.0f) ? 4 : 5;
    }
    tile_index += face;
  }
  return shadow_tile_sample(tile_index, P, N, length(to_p));
}
