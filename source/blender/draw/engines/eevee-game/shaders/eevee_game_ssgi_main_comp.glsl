/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Screen-Space Global Illumination: trace (half resolution).
 *
 * One diffuse bounce of the previous frame's opaque radiance. Each pixel marches a few cosine
 * distributed rays in world space against the depth buffer. A ray that hits takes the radiance
 * of the hit point, reprojected into the previous frame; a ray that misses sees the world, which
 * the lighting pass already has. The output is therefore:
 *   rgb: sum of the hit radiance / ray count,
 *   a:   fraction of the rays that hit,
 * and the lighting pass replaces that fraction of the world diffuse light with the bounce.
 * Replacing instead of adding keeps the energy right: the bounce is what the occluder sends in
 * place of the world it hides.
 *
 * The noise is spatial only and fixed in time: without temporal accumulation a per-frame
 * rotation would only flicker. The bilateral pass after this one smooths it.
 *
 * Local group 8x8.
 */

#include "infos/eevee_game_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_game_ssgi_main)

#include "eevee_gbuffer_read_lib.glsl"
#include "eevee_reverse_z_lib.glsl"

#define SSGI_HIT_LUMA_MAX 1.0f

float linear_depth(float depth)
{
  return (z_planes.x * z_planes.y) / (z_planes.y - depth * (z_planes.y - z_planes.x));
}

float3 reconstruct_world_pos(float2 uv, float depth)
{
  float4 ws = viewprojinv * float4(uv * 2.0f - 1.0f, depth * 2.0f - 1.0f, 1.0f);
  return ws.xyz / ws.w;
}

/* Screen UV and window depth of a world position. */
float3 world_to_screen(float4x4 vp, float3 ws)
{
  float4 clip = vp * float4(ws, 1.0f);
  return (clip.xyz / clip.w) * 0.5f + 0.5f;
}

/* Interleaved gradient noise (Jimenez 2014). */
float ign(float2 px)
{
  return fract(52.9829189f * fract(dot(px, float2(0.06711056f, 0.00583715f))));
}

float3 cosine_direction(float2 u, float3 N)
{
  float sin_theta = sqrt(u.x);
  float cos_theta = sqrt(1.0f - u.x);
  float phi = u.y * 6.2831853f;
  float3 up = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
  float3 T = normalize(cross(up, N));
  float3 B = cross(N, T);
  return T * (sin_theta * cos(phi)) + B * (sin_theta * sin(phi)) + N * cos_theta;
}

void main()
{
  const int2 out_px = int2(gl_GlobalInvocationID.xy);
  const int2 out_res = imageSize(out_ssgi_img);
  if (any(greaterThanEqual(out_px, out_res))) {
    return;
  }

  /* Top left texel of the 2x2 block: the G-Buffer and the depth are read at the same texel. */
  const int2 texel = min(out_px * 2, int2(screen_res) - 1);
  const float2 uv = (float2(texel) + 0.5f) / screen_res;

  float depth = reverse_z::read(texelFetch(depth_tx, texel, 0).r);
  /* Sky, or a surface without closure (emission only): nothing to light. A pixel without
   * closure never had its normal layer written. */
  if (depth >= 1.0f || gbuffer::read_header(texel).is_empty()) {
    imageStore(out_ssgi_img, out_px, float4(0.0f));
    return;
  }

  const float3 P = reconstruct_world_pos(uv, depth);
  const float3 N = gbuffer::read_normal(texel);
  const float view_dist = linear_depth(depth);
  /* Away from the surface, against self intersection on the depth quantization. */
  const float3 origin = P + N * (0.002f * view_dist + 0.01f);

  const int RAY_COUNT = 4;
  const float2 noise = float2(ign(float2(out_px)), ign(float2(out_px) + float2(17.0f, 59.0f)));

  float3 radiance = float3(0.0f);
  float hits = 0.0f;
  for (int r = 0; r < RAY_COUNT; r++) {
    /* Stratified on the first dimension, rotated per pixel. */
    float2 u = fract(float2((float(r) + noise.x) / float(RAY_COUNT),
                            noise.y + float(r) * 0.618034f));
    float3 dir = cosine_direction(u, N);

    for (int i = 0; i < ssgi_quality_steps; i++) {
      /* Quadratic distribution: dense near the surface, where contact matters. */
      float s = (float(i) + noise.x) / float(ssgi_quality_steps);
      float t = ssgi_radius * max(s * s, 0.02f);
      float3 ray_ws = origin + dir * t;
      float3 screen = world_to_screen(viewproj, ray_ws);
      if (any(lessThan(screen.xy, float2(0.0f))) || any(greaterThan(screen.xy, float2(1.0f))) ||
          screen.z >= 1.0f)
      {
        break; /* Off screen: the world is seen. */
      }
      /* The opaque depth, conventional range, at a mip whose texel covers about an eighth of
       * the screen distance marched: neighbouring threads share texels instead of thrashing
       * the cache. The coarser mips hold the farthest depth, which only loses thin hits. */
      float march_px = length((screen.xy - uv) * screen_res);
      float mip = clamp(log2(max(march_px, 1.0f)) - 3.0f, 0.0f, 5.0f);
      float scene_depth = textureLod(hiz_tx, screen.xy * hiz_uv_scale, mip).r;
      if (scene_depth >= 1.0f) {
        continue;
      }
      float delta = linear_depth(screen.z) - linear_depth(scene_depth);
      if (delta > 0.0f) {
        /* Behind the depth buffer. A thin layer counts as a hit, more is a ray passing behind
         * an object, which the depth buffer cannot tell from a solid. */
        if (delta < 0.25f + t * 0.5f) {
          float3 prev = world_to_screen(viewproj_prev, ray_ws);
          if (all(greaterThan(prev.xy, float2(0.0f))) && all(lessThan(prev.xy, float2(1.0f)))) {
            float3 hit = textureLod(scene_color_tx, prev.xy, 0.0f).rgb;
            float luma = dot(hit, float3(0.2126f, 0.7152f, 0.0722f));
            /* Firefly clamp: a small bright emitter is found by one ray in many, and four rays
             * per pixel cannot average that out. Losing some of its bounce is the lesser evil. */
            hit *= min(1.0f, SSGI_HIT_LUMA_MAX / max(luma, 1e-6f));
            luma = min(luma, SSGI_HIT_LUMA_MAX);
            radiance += mix(float3(luma), hit, ssgi_color_saturation);
            hits += 1.0f;
          }
        }
        break;
      }
    }
  }

  const float inv = 1.0f / float(RAY_COUNT);
  imageStore(out_ssgi_img, out_px, float4(radiance * (inv * ssgi_intensity), hits * inv));
}
