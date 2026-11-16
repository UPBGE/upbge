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

  uchar closure_count = gbuf.header.closure_len();

  float3 radiance = float3(0.0f);
  for (uchar i = 0; i < GBUFFER_LAYER_MAX && i < closure_count; i++) {
    ClosureUndetermined cl = gbuf.layer_get(i);

    float3 light_sum = float3(0.0f);
    for (int l = 0; l < light_count; l++) {
      LightData light = light_buf[l];

      float3 L;
      float3 incoming = light_incoming(light, P, L);

      /* The mask is the filtered shadow of the sun cascades only. */
      if (light.type == LIGHT_TYPE_SUN && light.shadow_index >= 0) {
        incoming *= sun_shadow;
      }

      float lobe = 0.0f;
      switch (cl.type) {
        case CLOSURE_BSSRDF_BURLEY_ID:
        case CLOSURE_BSDF_DIFFUSE_ID:
          /* Lambert: a constant 1 / pi in every direction. */
          lobe = saturate(dot(cl.N, L)) * M_1_PI;
          break;
        case CLOSURE_BSDF_TRANSLUCENT_ID:
          /* Lit from behind. */
          lobe = saturate(-dot(cl.N, L)) * M_1_PI;
          break;
        case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
          lobe = ggx_lobe(cl.N, L, V, to_closure_reflection(cl).roughness);
          break;
        default:
          /* Refraction needs what is behind the surface; no direct term. */
          break;
      }
      light_sum += incoming * lobe;
    }

    /* Refraction gets no ambient either: it would turn glass opaque. */
    if (cl.type != CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID) {
      light_sum += indirect;
    }

    radiance += light_sum * cl.color;
  }

  /* Alpha is transmittance and the blend is additive: adding zero keeps what
   * the G-Buffer pass wrote. */
  out_radiance = float4(radiance, 0.0f);
}
