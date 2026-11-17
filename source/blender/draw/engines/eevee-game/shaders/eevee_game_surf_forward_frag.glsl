/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Forward surface shading for the materials using the blended render method
 * (alpha blended transparency, and opaque forward materials).
 *
 * EEVEE's generated node tree evaluates the closures, the game light loop of
 * `eevee_game_light_lib.glsl` lights them: the same lights, shadows and lobes
 * as the deferred pass, plus the world lighting. Written premultiplied:
 * the blend is `dst = src.rgb + dst * (1 - src.a)` with
 * src.a = 1 - average transmittance (monochromatic transmittance).
 */

#include "infos/eevee_game_infos.hh"
#include "infos/eevee_geom_infos.hh"
#include "infos/eevee_nodetree_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(eevee_nodetree)
FRAGMENT_SHADER_CREATE_INFO(eevee_geom_mesh)
FRAGMENT_SHADER_CREATE_INFO(eevee_game_surf_forward)

#include "draw_curves_lib.glsl"
#include "draw_view_lib.glsl"
#include "eevee_game_light_lib.glsl"
#include "eevee_nodetree_frag_lib.glsl"
#include "eevee_sampling_lib.glsl"
#include "eevee_surf_lib.glsl"

/* Radiance leaving the surface toward the viewer: game lights and world on
 * the closures of the node tree, emission. */
float3 game_forward_radiance()
{
  float3 radiance = g_emission;

#ifdef LIGHT_CLOSURE_EVAL_COUNT
  float3 P = g_data.P;
  float3 V = drw_world_incident_vector(P);
  float view_depth = drw_view_z_distance(P);

  ClosureUndetermined cl[LIGHT_CLOSURE_EVAL_COUNT];
  float3 light_sum[LIGHT_CLOSURE_EVAL_COUNT];
  for (int i = 0; i < LIGHT_CLOSURE_EVAL_COUNT; i++) {
    /* Color already scaled by the closure weight. */
    cl[i] = g_closure_get_resolved(uchar(i), 1.0f);
    light_sum[i] = float3(0.0f);
  }

  /* Lights outside, shadows inside: one shadow lookup per light, shared by
   * every closure. The geometric normal offsets the shadow lookup, it does
   * not flip with normal maps. */
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
        continue;
      }
      if (light.shadow_index >= 0) {
        incoming *= game_light_shadow(light, P, g_data.Ng, view_depth);
      }

      for (int i = 0; i < LIGHT_CLOSURE_EVAL_COUNT; i++) {
        if (cl[i].weight <= CLOSURE_WEIGHT_CUTOFF) {
          continue;
        }
        float lobe = 0.0f;
        switch (cl[i].type) {
          case CLOSURE_BSSRDF_BURLEY_ID:
          case CLOSURE_BSDF_DIFFUSE_ID:
            lobe = saturate(dot(cl[i].N, L)) * M_1_PI;
            break;
          case CLOSURE_BSDF_TRANSLUCENT_ID:
            lobe = saturate(-dot(cl[i].N, L)) * M_1_PI;
            break;
          case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
            lobe = ggx_lobe(cl[i].N, L, V, to_closure_reflection(cl[i]).roughness);
            break;
          default:
            /* Refraction: what is behind comes through the transmittance. */
            break;
        }
        light_sum[i] += incoming * lobe;
      }
    }
  }

  for (int i = 0; i < LIGHT_CLOSURE_EVAL_COUNT; i++) {
    if (cl[i].weight <= CLOSURE_WEIGHT_CUTOFF) {
      continue;
    }
    /* World light, as in the deferred pass (no AO here). */
    switch (cl[i].type) {
      case CLOSURE_BSSRDF_BURLEY_ID:
      case CLOSURE_BSDF_DIFFUSE_ID:
        light_sum[i] += world_diffuse(cl[i].N);
        break;
      case CLOSURE_BSDF_TRANSLUCENT_ID:
        light_sum[i] += world_diffuse(-cl[i].N);
        break;
      case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
        light_sum[i] += world_specular(reflect(-V, cl[i].N),
                                       to_closure_reflection(cl[i]).roughness);
        break;
      default:
        /* Refraction: what is behind comes through the transmittance. */
        break;
    }
    radiance += light_sum[i] * cl[i].color;
  }
#endif

  return radiance;
}

float4 closure_to_rgba(Closure /*cl_unused*/)
{
  float4 out_color = float4(game_forward_radiance(),
                            saturate(1.0f - average(g_transmittance)));

  /* Reset for the next closure tree. */
  float noise = utility_tx_fetch(utility_tx, gl_FragCoord.xy, UTIL_BLUE_NOISE_LAYER).r;
  float closure_rand = fract(noise + sampling_rng_1D_get(SAMPLING_CLOSURE));
  closure_weights_reset(closure_rand);

  return out_color;
}

void main()
{
  init_globals();

  float noise = utility_tx_fetch(utility_tx, gl_FragCoord.xy, UTIL_BLUE_NOISE_LAYER).r;
  float closure_rand = fract(noise + sampling_rng_1D_get(SAMPLING_CLOSURE));

  fragment_displacement();

  nodetree_surface(closure_rand);

  float3 radiance = game_forward_radiance();

  eObjectInfoFlag ob_flag = drw_object_infos().flag;
  if (flag_test(ob_flag, OBJECT_HOLDOUT)) {
    g_holdout = 1.0f - average(g_transmittance);
  }
  g_holdout = saturate(g_holdout);
  radiance *= 1.0f - g_holdout;

  float coverage = saturate(1.0f - average(g_transmittance));

  /* Fog between the camera and the surface, already integrated in the froxel
   * grid (the resolve pass gave the same to the opaque pixels). The surface
   * light is dimmed by the transmittance T in front of it. Under it, the
   * color composited earlier already holds the in-scatter S of that same
   * segment; the blend keeps (1 - coverage) of it, so the surface adds back
   * coverage * S. Premultiplied result: T C + coverage S. */
  if (game_lighting_buf.use_volume) {
    float z = -drw_point_world_to_view(g_data.P).z;
    float w = saturate(log(max(z, game_lighting_buf.volume_z_near) /
                           game_lighting_buf.volume_z_near) *
                       game_lighting_buf.volume_log_inv);
    float2 uv = gl_FragCoord.xy / float2(game_lighting_buf.render_extent);
    float4 fog = texture(volume_tx, float3(uv, w));
    radiance = radiance * fog.a + fog.rgb * coverage;
  }

  out_radiance = float4(radiance, coverage);
}
