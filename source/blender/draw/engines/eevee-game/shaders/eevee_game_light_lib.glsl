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
float ggx_lobe(float3 N, float3 L, float3 V, float roughness, float light_size)
{
  float NL = dot(N, L);
  if (NL <= 0.0f) {
    return 0.0f;
  }
  float NV = max(dot(N, V), 1e-4f);
  float3 H = normalize(L + V);
  float NH = clamp(dot(N, H), 0.0f, 1.0f);

  /* Perceptual roughness squared, floored: a perfect mirror has an
   * infinitely narrow lobe that a punctual light can never hit. */
  float a = max(roughness * roughness, 2e-3f);
  float a2 = a * a;

  /* D: fraction of microfacets whose normal is H, seen through the disc of
   * the light rather than a point: the lobe widened by a third of the sine
   * of the light's angular radius (Karis 2013, "Real Shading in Unreal
   * Engine 4", eq. 10). D integrates to one at any width, so a large light
   * spreads its highlight and dims it instead of adding energy. The
   * visibility term keeps the surface's own roughness. */
  float a_light = min(a + light_size * (1.0f / 3.0f), 1.0f);
  float a2_light = a_light * a_light;
  float d = (NH * a2_light - NH) * NH + 1.0f;
  float D = a2_light / (M_PI * d * d);

  /* Height correlated Smith visibility, with the 1 / (4 NL NV) of the
   * microfacet BRDF already folded in. */
  float vis_v = NL * sqrt(NV * (NV - NV * a2) + a2);
  float vis_l = NV * sqrt(NL * (NL - NL * a2) + a2);
  float vis = 0.5f / max(vis_v + vis_l, 1e-5f);

  return D * vis * NL;
}

/* Light arriving at P from one light: direction towards the light, the
 * irradiance on a surface facing it, and the sine of the angular radius of
 * the emitter seen from P. The color is already the intensity (see
 * LightData::color). */
float3 light_incoming(GameLightData light, float3 P, out float3 L, out float size)
{
  if (light.type == LIGHT_TYPE_SUN) {
    L = -light.direction;
    size = light.radius;
    return light.color;
  }

  float3 to_light = light.position - P;
  float dist_sqr = max(dot(to_light, to_light), 1e-4f);
  float inv_dist = inversesqrt(dist_sqr);
  L = to_light * inv_dist;
  size = min(light.radius * inv_dist, 1.0f);

  /* Inverse square falloff, windowed so it reaches exactly zero at the
   * light's range. A bounded light is what lets it be culled later. */
  float range_sqr = light.attenuation * light.attenuation;
  float window = clamp(1.0f - square(dist_sqr / max(range_sqr, 1e-4f)), 0.0f, 1.0f);
  float falloff = square(window) / dist_sqr;

  if (light.type == LIGHT_TYPE_SPOT) {
    /* EEVEE's cone, smoothstep((cos(theta) - cos_outer) / band), written in
     * 1 - cos units where the halves are precise. */
    float2 spot = unpackHalf2x16(light.spot_shape);
    float x = 1.0f - dot(-L, light.direction);
    falloff *= smoothstep(0.0f, 1.0f, (spot.x - x) / max(spot.y, 1e-7f));
  }
  else if (light.type == LIGHT_TYPE_AREA_RECT || light.type == LIGHT_TYPE_AREA_ELLIPSE) {
    /* One sided Lambertian panel seen from far enough to be its center:
     * I(theta) = I0 cos(theta), I0 in the color. Near a large panel this
     * overestimates, the real emitter being spread out. */
    falloff *= max(dot(-L, light.direction), 0.0f);
  }

  return light.color * falloff;
}

float3 light_incoming(GameLightData light, float3 P, out float3 L)
{
  float size;
  return light_incoming(light, P, L, size);
}

/* World light reaching a Lambertian surface of normal N, as the radiance a
 * white diffuse surface reflects: L1 spherical harmonics, clamped because a
 * strongly directional sky rings below zero on the far side. */
float3 world_diffuse(float3 N)
{
  float4 n = float4(1.0f, N);
  return max(float3(dot(world_sh_buf[0], n), dot(world_sh_buf[1], n), dot(world_sh_buf[2], n)),
             float3(0.0f));
}

/* World radiance seen in the mirror direction R, pre-filtered for the GGX
 * lobe of the given perceptual roughness. */
float3 world_specular(float3 R, float roughness)
{
  return textureLod(world_radiance_tx, R, roughness * WORLD_CUBE_LOD_MAX).rgb;
}

/* First word of the light mask of the screen tile holding a fragment. The
 * set bits of the `word_count` words from there are the lights to walk. */
int light_tile_mask_base(float2 frag_coord)
{
  int2 tile = int2(frag_coord) / LIGHT_TILE_SIZE;
  return (tile.y * game_lighting_buf.tile_count.x + tile.x) * game_lighting_buf.word_count;
}

/* Lit fraction of P in one shadow face: 3x3 PCF. dist is the distance to
 * the light for a perspective face (texel size grows with it), ignored for
 * an orthographic cascade. */
float shadow_tile_sample(int tile_index, float3 P, float3 N, float dist)
{
  GameShadowTileData tile = shadow_tile_buf[tile_index];
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

/* Shadow face of a shadowed light (shadow_index >= 0) holding P, or -1 past
 * the sun's shadow distance. view_depth is the positive view space depth of
 * P, used to pick the sun cascade. */
int game_light_shadow_tile(GameLightData light, float3 P, float view_depth)
{
  if (light.type == LIGHT_TYPE_SUN) {
    /* Four cascades, the first whose split covers the depth. */
    for (int c = 0; c < 4; c++) {
      int tile_index = light.shadow_index + c;
      if (view_depth < shadow_tile_buf[tile_index].split_depth) {
        return tile_index;
      }
    }
    return -1;
  }

  float3 to_p = P - light.position;
  int tile_index = light.shadow_index;
  if (light.type != LIGHT_TYPE_SPOT) {
    /* Point and area lights: cube faces ordered +X -X +Y -Y +Z -Z. An area
     * light skips the faces behind it, which no lit point selects. */
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
  return tile_index;
}

/* Lit fraction of P for a shadowed light (shadow_index >= 0). */
float game_light_shadow(GameLightData light, float3 P, float3 N, float view_depth)
{
  int tile_index = game_light_shadow_tile(light, P, view_depth);
  if (tile_index < 0) {
    return 1.0f;
  }
  if (light.type != LIGHT_TYPE_SUN) {
    /* Bias: the receiver moves toward the light, which removes the self shadowing of surfaces
     * at a grazing angle or with layers closer than the normal offset. */
    float3 to_light = light.position - P;
    float dist = length(to_light);
    float bias = shadow_tile_buf[tile_index].bias;
    return shadow_tile_sample(
        tile_index, P + to_light * (min(bias, dist * 0.5f) / max(dist, 1e-6f)), N, dist);
  }
  float shadow = shadow_tile_sample(tile_index, P, N, 0.0f);
  /* Over the end of its range a cascade fades into the next one, the last
   * into no shadow: the texel size jumps by the split ratio there, and a
   * seam shows where a blend does not. Two lookups in the band only. */
  float split = shadow_tile_buf[tile_index].split_depth;
  float blend = clamp((view_depth / split - (1.0f - SHADOW_CASCADE_BLEND)) *
                          (1.0f / SHADOW_CASCADE_BLEND),
                      0.0f,
                      1.0f);
  if (blend > 0.0f) {
    float next = (tile_index < light.shadow_index + 3) ?
                     shadow_tile_sample(tile_index + 1, P, N, 0.0f) :
                     1.0f;
    shadow = mix(shadow, next, blend);
  }
  return shadow;
}

/* Same for a point in a medium: one bilinear compare and no normal offset.
 * There is no surface to get acne on, and a froxel is far larger than the
 * 3x3 kernel anyway; this is the cost of every light in every froxel. */
float game_light_shadow_volume(GameLightData light, float3 P, float view_depth)
{
  int tile_index = game_light_shadow_tile(light, P, view_depth);
  if (tile_index < 0) {
    return 1.0f;
  }
  GameShadowTileData tile = shadow_tile_buf[tile_index];
  float4 clip = tile.view_proj * float4(P, 1.0f);
  if (clip.w <= 0.0f) {
    return 1.0f;
  }
  float3 ndc = clip.xyz / clip.w;
  if (any(greaterThan(abs(ndc.xy), float2(1.0f)))) {
    return 1.0f;
  }
  float3 tap = float3(tile.atlas_rect.xy + (ndc.xy * 0.5f + 0.5f) * tile.atlas_rect.zw,
                      0.5f - 0.5f * ndc.z);
  float occluded = (tile.atlas == 0) ? textureLod(shadow_static_atlas, tap, 0.0f) :
                                       textureLod(shadow_dynamic_atlas, tap, 0.0f);
  return 1.0f - occluded;
}
