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
#include "eevee_closure_lib.glsl"
#include "eevee_game_light_lib.glsl"
#include "eevee_game_ssr_lib.glsl"
#include "eevee_nodetree_frag_lib.glsl"
#include "eevee_sampling_lib.glsl"
#include "eevee_surf_lib.glsl"

/* Thickness of the material at this fragment, for the refraction direction. */
Thickness g_game_thickness;

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
      GameLightData light = light_buf[l];

      float3 L;
      float size;
      float3 incoming = light_incoming(light, P, L, size);
      if (all(equal(incoming, float3(0.0f))) || light.diffuse_specular == 0u) {
        continue;
      }
      float2 lobe_factor = unpackHalf2x16(light.diffuse_specular);
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
            lobe = clamp(dot(cl[i].N, L), 0.0f, 1.0f) * (M_1_PI * lobe_factor.x);
            break;
          case CLOSURE_BSDF_TRANSLUCENT_ID:
            lobe = clamp(-dot(cl[i].N, L), 0.0f, 1.0f) * (M_1_PI * lobe_factor.x);
            break;
          case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID:
            lobe = ggx_lobe(cl[i].N, L, V, to_closure_reflection(cl[i]).roughness, size) *
                   lobe_factor.y;
            break;
          default:
            /* Refraction: what is behind comes through the transmittance. */
            break;
        }
        light_sum[i] += incoming * lobe;
      }
    }
  }

  /* The screen-space reflection is traced for the first reflection closure only, as in the
   * deferred pass. */
  bool reflection_used = false;
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
      case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID: {
        float roughness = to_closure_reflection(cl[i]).roughness;
        float3 specular = world_specular(reflect(-V, cl[i].N), roughness);
        if (game_lighting_buf.use_ssr && !reflection_used) {
          reflection_used = true;
          /* The opaque radiance was copied after the opaque lighting of this frame, with
           * this camera: no reprojection. */
          const float4x4 viewproj = drw_view().winmat * drw_view().viewmat;
          const float4x4 viewprojinv = drw_view().viewinv * drw_view().wininv;
          float noise = fract(52.9829189f * fract(dot(gl_FragCoord.xy,
                                                      float2(0.06711056f, 0.00583715f))));
          float4 reflection = ssr_trace(P,
                                        cl[i].N,
                                        roughness,
                                        noise,
                                        drw_view_position(),
                                        viewproj,
                                        viewprojinv,
                                        viewproj,
                                        float2(game_lighting_buf.volume_z_near,
                                               game_lighting_buf.z_far),
                                        float2(game_lighting_buf.render_extent),
                                        uniform_buf.hiz.uv_scale,
                                        game_lighting_buf.ssr_max_steps,
                                        game_lighting_buf.ssr_thickness,
                                        game_lighting_buf.ssr_max_roughness);
          specular = specular * (1.0f - reflection.a) + reflection.rgb;
        }
        light_sum[i] += specular;
        break;
      }
      case CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID: {
        /* As EEVEE without ray traced refraction: the world seen through the surface, along the
         * refracted direction (through the thickness sphere or slab when there is one). */
        LightProbeRay ray = bxdf_lightprobe_ray(cl[i], P, V, g_game_thickness);
        light_sum[i] += world_specular(ray.dominant_direction, ray.perceptual_roughness);
        if (g_game_thickness.value() != 0.0f) {
          /* Two transmission events: the surface color applies twice. */
          cl[i].color *= cl[i].color;
        }
        break;
      }
      default:
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
                            clamp(1.0f - average(g_transmittance), 0.0f, 1.0f));

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

  g_game_thickness = Thickness::from(nodetree_thickness(), thickness_mode);

  nodetree_surface(closure_rand);

  float3 radiance = game_forward_radiance();

  eObjectInfoFlag ob_flag = drw_object_infos().flag;
  if (flag_test(ob_flag, OBJECT_HOLDOUT)) {
    g_holdout = 1.0f - average(g_transmittance);
  }
  g_holdout = clamp(g_holdout, 0.0f, 1.0f);
  radiance *= 1.0f - g_holdout;

  float coverage = clamp(1.0f - average(g_transmittance), 0.0f, 1.0f);

  /* Fog between the camera and the surface, already integrated in the froxel
   * grid (the resolve pass gave the same to the opaque pixels). The surface
   * light is dimmed by the transmittance T in front of it, per channel.
   * Under it, the color composited earlier already holds the in-scatter S of
   * that same segment; the blend keeps (1 - coverage) of it, so the surface
   * adds back coverage * S. Premultiplied result: T C + coverage S. */
  if (game_lighting_buf.use_volume) {
    float z = -drw_point_world_to_view(g_data.P).z;
    float w = clamp(log(max(z, game_lighting_buf.volume_z_near) /
                           game_lighting_buf.volume_z_near) *
                        game_lighting_buf.volume_log_inv,
                    0.0f,
                    1.0f);
    float2 uv = gl_FragCoord.xy / float2(game_lighting_buf.render_extent);
    float3 uvw = float3(uv, w);
    radiance = radiance * texture(volume_transmittance_tx, uvw).rgb +
               texture(volume_tx, uvw).rgb * coverage;
  }

  out_radiance = float4(radiance, coverage);
}
