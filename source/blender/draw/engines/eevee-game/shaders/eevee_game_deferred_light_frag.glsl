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

  /* Indirect light: one screen-space bounce plus a flat ambient term, both
   * attenuated by AO. The ambient color stands in for image based lighting
   * until there are probes. */
  float3 indirect = (bounce + game_lighting_buf.world_ambient) * ao;

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
                                                     game_light_shadow(light, P, cl[0].N, 0.0f);
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
