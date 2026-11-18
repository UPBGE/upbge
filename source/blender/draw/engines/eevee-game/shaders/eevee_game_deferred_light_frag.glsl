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
 * The lights walked are those the tiled light culling found in the pixel's
 * screen tile.
 */

#include "infos/eevee_game_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_game_deferred_light)

#include "draw_view_lib.glsl"
#include "eevee_game_light_lib.glsl"
#include "eevee_gbuffer_read_lib.glsl"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_math_constants_lib.glsl"

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


  int closure_count = min(int(gbuf.header.closure_len()), GBUFFER_LAYER_MAX);
  ClosureUndetermined cl[GBUFFER_LAYER_MAX];
  float3 light_sum[GBUFFER_LAYER_MAX];
  for (int i = 0; i < closure_count; i++) {
    cl[i] = gbuf.layer_get(uchar(i));
    light_sum[i] = float3(0.0f);
  }

  /* Lights outside, shadows inside: the shadow lookup is the expensive
   * part and is shared by every closure of the pixel. Only the lights the
   * culling found in this tile are walked, by bit. */
  int tile_base = light_tile_mask_base(gl_FragCoord.xy);
  for (int w = 0; w < game_lighting_buf.word_count; w++) {
    uint mask = light_tile_mask_buf[tile_base + w];
    while (mask != 0u) {
      int l = w * 32 + findLSB(mask);
      mask &= mask - 1u;
      GameLightData light = light_buf[l];

      float3 L;
      float size;
      float3 incoming = light_incoming(light, P, L, size);
      /* Out of range, outside the cone, or a light for volumes only: no
       * shadow lookup. */
      if (all(equal(incoming, float3(0.0f))) || light.diffuse_specular == 0u) {
        continue;
      }
      float2 lobe_factor = unpackHalf2x16(light.diffuse_specular);

      if (light.shadow_index >= 0) {
        /* The sun shadow comes filtered from the PCF compute, the punctual
         * ones are looked up here, offset along the first closure normal. */
        incoming *= (light.type == LIGHT_TYPE_SUN) ? sun_shadow :
                                                     game_light_shadow(light, P, cl[0].N, 0.0f);
      }

      for (int i = 0; i < closure_count; i++) {
        float lobe = 0.0f;
        switch (cl[i].type) {
          case CLOSURE_BSSRDF_BURLEY_ID:
          case CLOSURE_BSDF_DIFFUSE_ID:
            /* Lambert: a constant 1 / pi in every direction. */
            lobe = clamp(dot(cl[i].N, L), 0.0f, 1.0f) * (M_1_PI * lobe_factor.x);
            break;
          case CLOSURE_BSDF_TRANSLUCENT_ID:
            /* Lit from behind. */
            lobe = clamp(-dot(cl[i].N, L), 0.0f, 1.0f) * (M_1_PI * lobe_factor.x);
            break;
          case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
            lobe = ggx_lobe(cl[i].N, L, V, to_closure_reflection(cl[i]).roughness, size) *
                   lobe_factor.y;
            break;
          default:
            /* Refraction needs what is behind the surface; no direct term. */
            break;
        }
        light_sum[i] += incoming * lobe;
      }
    }
  }

  /* Indirect light: the world, plus one screen-space bounce on the diffuse
   * lobes, attenuated by AO (also standing in for specular occlusion). */
  float3 radiance = float3(0.0f);
  for (int i = 0; i < closure_count; i++) {
    switch (cl[i].type) {
      case CLOSURE_BSSRDF_BURLEY_ID:
      case CLOSURE_BSDF_DIFFUSE_ID:
        light_sum[i] += (bounce + world_diffuse(cl[i].N)) * ao;
        break;
      case CLOSURE_BSDF_TRANSLUCENT_ID:
        light_sum[i] += world_diffuse(-cl[i].N) * ao;
        break;
      case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
        light_sum[i] += world_specular(reflect(-V, cl[i].N),
                                       to_closure_reflection(cl[i]).roughness) *
                        ao;
        break;
      case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID: {
        /* The world seen through the surface, as EEVEE without ray tracing
         * (bxdf_ggx_lightprobe_transmission): an object with thickness
         * bends the ray in and out, approximated by the inverse ratio. */
        ClosureRefraction refraction = to_closure_refraction(cl[i]);
        float ior = refraction.ior;
        /* EEVEE's curve fit of the apparent blur of GGX refraction. */
        float roughness = refraction.roughness *
                          sqrt((ior > 1.0f) ? (1.0f - 1.0f / ior) :
                                              (clamp(1.0f - ior, 0.0f, 1.0f) * 0.64f));
        if (gbuffer::read_thickness(gbuf.header, texel).value() != 0.0f) {
          ior = 1.0f / ior;
        }
        /* Dominant direction: from the refracted ray toward the inner
         * normal as the lobe widens. Total internal reflection leaves the
         * inner normal. */
        float m = roughness * roughness;
        float smoothness = 1.0f - m;
        float3 D = mix(-cl[i].N,
                       refract(-V, cl[i].N, 1.0f / ior),
                       smoothness * (sqrt(smoothness) + m));
        D = (dot(D, D) > 1e-8f) ? normalize(D) : -cl[i].N;
        light_sum[i] += world_specular(D, roughness);
        break;
      }
      default:
        break;
    }
    radiance += light_sum[i] * cl[i].color;
  }

  /* Alpha is transmittance and the blend is additive: adding zero keeps what
   * the G-Buffer pass wrote. */
  out_radiance = float4(radiance, 0.0f);
}
