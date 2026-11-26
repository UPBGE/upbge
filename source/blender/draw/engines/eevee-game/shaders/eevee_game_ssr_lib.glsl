/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/**
 * Screen-space reflection of one mirror ray, shared by the half resolution trace of the
 * deferred surfaces and the forward surfaces.
 *
 * Marches the reflected ray in screen space against the opaque depth (mip 0 of `hiz_tx`,
 * conventional range) and returns the opaque radiance of `scene_color_tx` at the hit:
 *   rgb: reflected radiance, already weighted by the confidence,
 *   a:   confidence: 0 keeps the world reflection, 1 is the traced one.
 * `viewproj_radiance` is the camera `scene_color_tx` was rendered with: the previous frame's
 * for the deferred trace, which runs before the lighting, the current one for the forward
 * surfaces, which run after the opaque radiance is copied.
 *
 * The march is linear in screen space, so every pixel of the path is visited once at most,
 * followed by a few bisection steps. `noise` in [0, 1) jitters the start against banding.
 */

float ssr_linear_depth(float depth, float2 z_planes)
{
  return (z_planes.x * z_planes.y) / (z_planes.y - depth * (z_planes.y - z_planes.x));
}

float3 ssr_screen_to_world(float4x4 viewprojinv, float2 uv, float depth)
{
  float4 ws = viewprojinv * float4(uv * 2.0f - 1.0f, depth * 2.0f - 1.0f, 1.0f);
  return ws.xyz / ws.w;
}

float4 ssr_trace(float3 P,
                 float3 N,
                 float roughness,
                 float noise,
                 float3 camera_pos,
                 float4x4 viewproj,
                 float4x4 viewprojinv,
                 float4x4 viewproj_radiance,
                 float2 z_planes,
                 float2 screen_res,
                 float2 hiz_uv_scale,
                 int max_steps,
                 float thickness,
                 float max_roughness)
{
  if (roughness >= max_roughness) {
    return float4(0.0f);
  }
  const float3 V = normalize(camera_pos - P);
  const float3 R = reflect(-V, N);

  /* Ray end in world space, then both ends in clip space. The end is pulled in front of the
   * near plane, where a perspective division would flip it. */
  const float view_dist = (viewproj * float4(P, 1.0f)).w;
  float4 h0 = viewproj * float4(P + N * (0.002f * view_dist), 1.0f);
  float ray_len = view_dist * 2.0f + 1.0f;
  float4 h1 = viewproj * float4(P + R * ray_len, 1.0f);
  if (h1.w < z_planes.x) {
    ray_len *= (h0.w - z_planes.x) / max(h0.w - h1.w, 1e-6f);
    h1 = viewproj * float4(P + R * ray_len, 1.0f);
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
  const float k0 = 1.0f / h0.w;
  const float k1 = 1.0f / h1.w;
  const float path_px = length(dir * t_max * screen_res);
  if (path_px < 2.0f) {
    return float4(0.0f);
  }
  const int steps = clamp(int(path_px / 4.0f), 4, max_steps);
  const float dt = t_max / float(steps);

  float t_prev = dt * 0.5f;
  float t_hit = -1.0f;
  /* The first step is one stride out: closer, the depth quantization of the surface itself
   * reads as a hit. */
  for (int i = 1; i < steps; i++) {
    float t = (float(i) + noise) * dt;
    float3 s = mix(s0, s1, t);
    float ray_lin = 1.0f / mix(k0, k1, t);
    float scene = textureLod(hiz_tx, s.xy * hiz_uv_scale, 0.0f).r;
    float delta = ray_lin - ssr_linear_depth(scene, z_planes);
    if (delta > 0.0f) {
      if (delta < thickness) {
        t_hit = t;
      }
      break;
    }
    t_prev = t;
  }
  if (t_hit < 0.0f) {
    return float4(0.0f);
  }
  /* Bisection between the last step in front and the hit. */
  float lo = t_prev;
  float hi = t_hit;
  for (int i = 0; i < 4; i++) {
    float t = 0.5f * (lo + hi);
    float3 s = mix(s0, s1, t);
    float scene = textureLod(hiz_tx, s.xy * hiz_uv_scale, 0.0f).r;
    if (1.0f / mix(k0, k1, t) > ssr_linear_depth(scene, z_planes)) {
      hi = t;
    }
    else {
      lo = t;
    }
  }
  const float3 s_hit = mix(s0, s1, hi);
  const float scene_hit = textureLod(hiz_tx, s_hit.xy * hiz_uv_scale, 0.0f).r;
  if (scene_hit >= 1.0f) {
    return float4(0.0f); /* Hit the sky: the world reflection is right. */
  }
  /* The hit, in the frame the radiance comes from. */
  const float3 hit_ws = ssr_screen_to_world(viewprojinv, s_hit.xy, scene_hit);
  float4 hp = viewproj_radiance * float4(hit_ws, 1.0f);
  float2 radiance_uv = (hp.xy / hp.w) * 0.5f + 0.5f;
  if (any(lessThan(radiance_uv, float2(0.0f))) || any(greaterThan(radiance_uv, float2(1.0f)))) {
    return float4(0.0f);
  }

  /* Confidence: away from the screen borders, for smooth surfaces, and not for rays that come
   * back toward the camera (they hit back faces the depth buffer does not have). */
  float2 border = min(s_hit.xy, 1.0f - s_hit.xy);
  float fade = smoothstep(0.0f, 0.08f, min(border.x, border.y));
  fade *= 1.0f - smoothstep(max_roughness * 0.8f, max_roughness, roughness);
  fade *= smoothstep(-0.1f, 0.1f, dot(R, -V));

  float3 radiance = textureLod(scene_color_tx, radiance_uv, 0.0f).rgb;
  return float4(radiance * fade, fade);
}
