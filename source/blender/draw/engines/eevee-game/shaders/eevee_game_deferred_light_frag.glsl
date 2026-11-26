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

/* View space z of a conventional depth buffer value, without a matrix inverse. */
float view_z_from_depth(float depth)
{
  const float4x4 winmat = drw_view().winmat;
  float ndc_z = depth * 2.0f - 1.0f;
  return drw_view_is_perspective() ? -winmat[3][2] / (ndc_z + winmat[2][2]) :
                                     (ndc_z - winmat[3][2]) / winmat[2][2];
}

/* The half resolution (radiance, weight) results of SSGI and SSR at a full resolution texel:
 * bilinear between the half resolution texels around it, weighted down where their depth
 * differs from the pixel's, so they do not bleed across depth edges. Half resolution texel q
 * was computed at full resolution texel 2q, its depth is mip 1 of the Hi-Z pyramid. The
 * weights are shared by both results. */
void half_res_upsample(int2 texel, float view_z, out float4 ssgi, out float4 ssr)
{
  const int2 half_max = textureSize(ssgi_tx, 0) - 1;
  const int2 base = texel >> 1;
  /* 0 on even pixels, which sit on a half resolution sample, 0.5 on odd ones. */
  const float2 f = float2(texel & 1) * 0.5f;
  /* Depth tolerance grows with the distance, like the depth quantization. */
  const float inv_sigma = 1.0f / (0.1f + 0.02f * abs(view_z));
  ssgi = float4(0.0f);
  ssr = float4(0.0f);
  float weight_sum = 0.0f;
  for (int i = 0; i < 4; i++) {
    int2 offset = int2(i & 1, i >> 1);
    float bilinear = ((offset.x != 0) ? f.x : 1.0f - f.x) * ((offset.y != 0) ? f.y : 1.0f - f.y);
    if (bilinear == 0.0f) {
      continue;
    }
    int2 q = min(base + offset, half_max);
    float delta = (view_z_from_depth(texelFetch(hiz_tx, q, 1).r) - view_z) * inv_sigma;
    float w = bilinear * (exp(-delta * delta) + 1e-4f);
    if (use_ssgi != 0) {
      ssgi += texelFetch(ssgi_tx, q, 0) * w;
    }
    if (use_ssr != 0) {
      ssr += texelFetch(ssr_tx, q, 0) * w;
    }
    weight_sum += w;
  }
  ssgi /= weight_sum;
  ssr /= weight_sum;
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
  /* rgb: bounce radiance, a: fraction of the world it hides. */
  float4 bounce, reflection;
  if (use_ssgi != 0 || use_ssr != 0) {
    half_res_upsample(texel, view_z_from_depth(depth), bounce, reflection);
  }
  else {
    bounce = reflection = float4(0.0f);
  }
  float sun_shadow = texelFetch(shadow_mask_tx, texel, 0).r;
  /* rgb: reflected radiance weighted by a, a: how much of the world reflection it replaces. */
  /* The trace is for the first reflection closure only. */
  bool reflection_used = false;


  /* The closure loops run to the constant GBUFFER_LAYER_MAX and skip the unused slots: with a
   * bound only known at run time the compiler cannot unroll them, and both arrays, indexed by
   * the loop counter, are spilled to local memory. That made this pass cost more than all the
   * lights it evaluates. */
  ClosureUndetermined cl[GBUFFER_LAYER_MAX];
  float3 light_sum[GBUFFER_LAYER_MAX];
  for (int i = 0; i < GBUFFER_LAYER_MAX; i++) {
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

      for (int i = 0; i < GBUFFER_LAYER_MAX; i++) {
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

  /* Indirect light: the world, of which the screen-space bounce replaces the part its rays
   * found occluded, on the diffuse lobes; attenuated by AO (also standing in for specular
   * occlusion). */
  float3 radiance = float3(0.0f);
  for (int i = 0; i < GBUFFER_LAYER_MAX; i++) {
    switch (cl[i].type) {
      case CLOSURE_BSSRDF_BURLEY_ID:
      case CLOSURE_BSDF_DIFFUSE_ID:
        light_sum[i] += (bounce.rgb + world_diffuse(cl[i].N) * (1.0f - bounce.a)) * ao;
        break;
      case CLOSURE_BSDF_TRANSLUCENT_ID:
        light_sum[i] += world_diffuse(-cl[i].N) * ao;
        break;
      case CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID: {
        float3 specular = world_specular(reflect(-V, cl[i].N),
                                         to_closure_reflection(cl[i]).roughness);
        if (!reflection_used) {
          specular = specular * (1.0f - reflection.a) + reflection.rgb;
          reflection_used = true;
        }
        light_sum[i] += specular * ao;
        break;
      }
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
