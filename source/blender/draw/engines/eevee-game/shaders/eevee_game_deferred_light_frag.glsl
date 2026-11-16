/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Deferred lighting.
 *
 * Decodes EEVEE's G-Buffer (written by the generated material shaders) and
 * lights it with the game light list. Drawn additively over combined_tx,
 * which already holds the emission written by the G-Buffer pass.
 *
 * Deliberately a plain loop over every light per pixel: no tiles, no
 * clusters. That is the right shape for a handful of lights, and it is one
 * buffer read per light with no indirection. Once profiling shows the light
 * count is what limits a scene, the loop body stays and only the range it
 * walks changes.
 */

#include "infos/eevee_game_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_game_deferred_light)

#include "draw_view_lib.glsl"
#include "eevee_gbuffer_read_lib.glsl"
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

/* Shadow of a punctual light: 3x3 PCF in the atlas face that sees P (one
 * face for a spot, the cube face of the dominant axis for a point light).
 * Returns the lit fraction. */
float punctual_shadow(LightData light, float3 P, float3 N)
{
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
  ShadowTileData tile = shadow_tile_buf[tile_index];

  /* Normal offset of 1.5 texels measured at the receiver: a texel of a
   * perspective face grows linearly with the distance to the light. It
   * removes acne without a depth bias that would detach contact shadows. */
  float texel_world = tile.texel_scale * length(to_p);
  float4 clip = tile.view_proj * float4(P + N * (1.5f * texel_world), 1.0f);
  if (clip.w <= 0.0f) {
    return 1.0f;
  }
  float3 ndc = clip.xyz / clip.w;
  if (any(greaterThan(abs(ndc.xy), float2(1.0f)))) {
    /* Outside the spot cone: the cone falloff already darkened it. */
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

void main()
{
  int2 texel = int2(gl_FragCoord.xy);

  const gbuffer::Layers gbuf = gbuffer::read_layers(texel);
  if (gbuf.has_no_closure()) {
    /* Background, or an emission only surface: nothing to light. */
    out_radiance = float4(0.0f);
    return;
  }

  /* Mip 0 of the Hi-Z pyramid is the opaque depth in conventional range. */
  float depth = texelFetch(hiz_tx, texel, 0).r;
  float3 P = drw_point_screen_to_world(float3(screen_uv, depth));
  float3 V = drw_world_incident_vector(P);

  float ao = (use_ao != 0) ? texelFetch(ao_tx, texel, 0).r : 1.0f;
  float3 bounce = (use_ssgi != 0) ? texelFetch(ssgi_tx, texel, 0).rgb : float3(0.0f);
  float sun_shadow = texelFetch(shadow_mask_tx, texel, 0).r;

  /* Indirect light: one screen-space bounce plus a flat ambient term, both
   * attenuated by AO. The ambient color stands in for image based lighting
   * until there are probes. */
  float3 indirect = (bounce + world_ambient) * ao;

  int closure_count = min(int(gbuf.header.closure_len()), GBUFFER_LAYER_MAX);
  ClosureUndetermined cl[GBUFFER_LAYER_MAX];
  float3 light_sum[GBUFFER_LAYER_MAX];
  for (int i = 0; i < closure_count; i++) {
    cl[i] = gbuf.layer_get(uchar(i));
    light_sum[i] = float3(0.0f);
  }

  /* Lights outside, shadows inside: the shadow lookup is the expensive
   * part and is shared by every closure of the pixel. */
  for (int l = 0; l < light_count; l++) {
    LightData light = light_buf[l];

    float3 L;
    float3 incoming = light_incoming(light, P, L);
    if (all(equal(incoming, float3(0.0f)))) {
      /* Out of range or outside the cone. */
      continue;
    }

    if (light.shadow_index >= 0) {
      /* The sun shadow comes filtered from the PCF compute, the punctual
       * ones are looked up here, offset along the first closure normal. */
      incoming *= (light.type == LIGHT_TYPE_SUN) ? sun_shadow :
                                                   punctual_shadow(light, P, cl[0].N);
    }

    for (int i = 0; i < closure_count; i++) {
      float lobe = 0.0f;
      switch (cl[i].type) {
        case CLOSURE_BSSRDF_BURLEY_ID:
        case CLOSURE_BSDF_DIFFUSE_ID:
          /* Lambert: a constant 1 / pi in every direction. */
          lobe = saturate(dot(cl[i].N, L)) * M_1_PI;
          break;
        case CLOSURE_BSDF_TRANSLUCENT_ID:
          /* Lit from behind. */
          lobe = saturate(-dot(cl[i].N, L)) * M_1_PI;
          break;
        case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
          lobe = ggx_lobe(cl[i].N, L, V, to_closure_reflection(cl[i]).roughness);
          break;
        default:
          /* Refraction needs what is behind the surface; no direct term. */
          break;
      }
      light_sum[i] += incoming * lobe;
    }
  }

  float3 radiance = float3(0.0f);
  for (int i = 0; i < closure_count; i++) {
    /* Refraction gets no ambient either: it would turn glass opaque. */
    if (cl[i].type != CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID) {
      light_sum[i] += indirect;
    }
    radiance += light_sum[i] * cl[i].color;
  }

  /* Alpha is transmittance and the blend is additive: adding zero keeps what
   * the G-Buffer pass wrote. */
  out_radiance = float4(radiance, 0.0f);
}
