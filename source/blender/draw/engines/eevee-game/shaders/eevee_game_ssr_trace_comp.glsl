/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Screen-space reflections: trace (half resolution).
 *
 * For the first GGX reflection closure of a pixel, smooth enough to be worth it, marches the
 * mirror direction in screen space against the opaque depth and returns the radiance of the
 * previous frame at the hit point, reprojected:
 *   rgb: reflected radiance, already weighted by the confidence,
 *   a:   confidence: 0 is the world reflection, 1 is the traced one.
 * The lighting pass blends the world reflection toward it. Rough surfaces fade out: one mirror
 * ray does not stand for a wide lobe.
 *
 * The march is linear in screen space, so every pixel of the path is visited once at most,
 * followed by a few bisection steps. The start is jittered per pixel against banding.
 *
 * Local group 8x8.
 */

#include "infos/eevee_game_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_game_ssr_trace)

#include "eevee_gbuffer_read_lib.glsl"
#include "eevee_reverse_z_lib.glsl"

float linear_depth(float depth)
{
  return (z_planes.x * z_planes.y) / (z_planes.y - depth * (z_planes.y - z_planes.x));
}

float3 reconstruct_world_pos(float2 uv, float depth)
{
  float4 ws = viewprojinv * float4(uv * 2.0f - 1.0f, depth * 2.0f - 1.0f, 1.0f);
  return ws.xyz / ws.w;
}

float4 project(float4x4 vp, float3 ws)
{
  return vp * float4(ws, 1.0f);
}

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
  imageStore(out_ssr_img, out_px, float4(0.0f));

  const int2 texel = min(out_px * 2, int2(screen_res) - 1);
  float depth = reverse_z::read(texelFetch(depth_tx, texel, 0).r);
  if (depth >= 1.0f) {
    return;
  }
  const gbuffer::Layers gbuf = gbuffer::read_layers(texel);
  if (gbuf.has_no_closure()) {
    return;
  }
  /* First reflection closure: the one the lighting pass blends. */
  float roughness = 2.0f;
  float3 N = float3(0.0f);
  int closure_count = min(int(gbuf.header.closure_len()), GBUFFER_LAYER_MAX);
  for (int i = 0; i < closure_count; i++) {
    ClosureUndetermined cl = gbuf.layer_get(uchar(i));
    if (cl.type == CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID) {
      roughness = to_closure_reflection(cl).roughness;
      N = cl.N;
      break;
    }
  }
  if (roughness >= ssr_max_roughness) {
    return;
  }

  const float2 uv = (float2(texel) + 0.5f) / screen_res;
  const float3 P = reconstruct_world_pos(uv, depth);
  const float3 V = normalize(camera_pos - P);
  const float3 R = reflect(-V, N);

  /* Ray end in world space, then both ends in clip space. The end is pulled in front of the
   * near plane, where a perspective division would flip it. */
  const float view_dist = linear_depth(depth);
  float ray_len = view_dist * 2.0f + 1.0f;
  float4 h0 = project(viewproj, P + N * (0.002f * view_dist));
  float4 h1 = project(viewproj, P + R * ray_len);
  if (h1.w < z_planes.x) {
    ray_len *= (h0.w - z_planes.x) / max(h0.w - h1.w, 1e-6f);
    h1 = project(viewproj, P + R * ray_len);
  }
  float3 s0 = (h0.xyz / h0.w) * 0.5f + 0.5f;
  float3 s1 = (h1.xyz / h1.w) * 0.5f + 0.5f;

  /* Clip the screen path to the screen. */
  float2 dir = s1.xy - s0.xy;
  float t_max = 1.0f;
  for (int a = 0; a < 2; a++) {
    if (dir[a] > 1e-6f) {
      t_max = min(t_max, (1.0f - s0[a]) / dir[a]);
    }
    else if (dir[a] < -1e-6f) {
      t_max = min(t_max, -s0[a] / dir[a]);
    }
  }
  /* Perspective correct interpolation: 1/w is linear in screen space, as is z/w. */
  const float k0 = 1.0f / h0.w, k1 = 1.0f / h1.w;
  const float path_px = length(dir * t_max * screen_res);
  if (path_px < 2.0f) {
    return;
  }
  const int steps = clamp(int(path_px / 4.0f), 4, ssr_max_steps);
  const float dt = t_max / float(steps);
  const float jitter = ign(float2(out_px));

  float t_prev = dt * 0.5f;
  float t_hit = -1.0f;
  /* The first step is one stride out: closer, the depth quantization of the surface itself
   * reads as a hit. */
  for (int i = 1; i < steps; i++) {
    float t = (float(i) + jitter) * dt;
    float3 s = mix(s0, s1, t);
    float ray_lin = 1.0f / mix(k0, k1, t);
    float scene = textureLod(hiz_tx, s.xy * hiz_uv_scale, 0.0f).r;
    float delta = ray_lin - linear_depth(scene);
    if (delta > 0.0f) {
      if (delta < ssr_thickness * ray_lin) {
        t_hit = t;
      }
      break;
    }
    t_prev = t;
  }
  if (t_hit < 0.0f) {
    return;
  }
  /* Bisection between the last step in front and the hit. */
  float lo = t_prev, hi = t_hit;
  for (int i = 0; i < 4; i++) {
    float t = 0.5f * (lo + hi);
    float3 s = mix(s0, s1, t);
    float scene = textureLod(hiz_tx, s.xy * hiz_uv_scale, 0.0f).r;
    if (1.0f / mix(k0, k1, t) > linear_depth(scene)) {
      hi = t;
    }
    else {
      lo = t;
    }
  }
  const float3 s_hit = mix(s0, s1, hi);
  const float scene_hit = textureLod(hiz_tx, s_hit.xy * hiz_uv_scale, 0.0f).r;
  if (scene_hit >= 1.0f) {
    return; /* Hit the sky: the world reflection is right. */
  }
  /* The hit, reprojected into the frame the radiance comes from. */
  const float3 hit_ws = reconstruct_world_pos(s_hit.xy, scene_hit);
  float4 hp = project(viewproj_prev, hit_ws);
  float2 prev_uv = (hp.xy / hp.w) * 0.5f + 0.5f;
  if (any(lessThan(prev_uv, float2(0.0f))) || any(greaterThan(prev_uv, float2(1.0f)))) {
    return;
  }

  /* Confidence: away from the screen borders, for smooth surfaces, and not for rays that come
   * back toward the camera (they hit back faces the depth buffer does not have). */
  float2 border = min(s_hit.xy, 1.0f - s_hit.xy);
  float fade = smoothstep(0.0f, 0.08f, min(border.x, border.y));
  fade *= 1.0f - smoothstep(ssr_max_roughness * 0.5f, ssr_max_roughness, roughness);
  fade *= smoothstep(-0.1f, 0.1f, dot(R, normalize(P - camera_pos)));

  float3 radiance = textureLod(scene_color_tx, prev_uv, 0.0f).rgb;
  imageStore(out_ssr_img, out_px, float4(radiance * fade, fade));
}
