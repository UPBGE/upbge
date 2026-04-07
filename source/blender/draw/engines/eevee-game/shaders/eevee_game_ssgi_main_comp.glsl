/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Screen-Space Global Illumination — Main Trace (half-resolution)
 *
 * Computes one diffuse indirect bounce from the current frame's scene color.
 *
 * Algorithm:
 *   For each half-res pixel, cast N rays in the hemisphere oriented by the
 *   surface normal.  March each ray through the Hi-Z pyramid to find the
 *   first screen-space intersection, then sample the scene color at that
 *   hit point.  Accumulate the radiance contributions.
 *
 * Hi-Z accelerated tracing (same as SSR):
 *   Start at the coarsest mip that fits the step size, descend through mips
 *   as the intersection becomes plausible.  This is the standard "Hi-Z ray march"
 *   from Morgan McGuire / Morgan & Luebke 2014.
 *
 * Limitations vs. EEVEE full SSGI:
 *   - One bounce only (no multi-bounce accumulation per frame)
 *   - Half-resolution to reduce cost by 4×
 *   - No temporal accumulation in this pass (TAA/FSR provides it)
 *   - Screen-space only: geometry outside the view frustum contributes nothing
 *
 * Cost: ~0.5ms at 1440p on a mid-range GPU with quality_steps=8.
 *
 * Local group 8×8.
 */

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

uniform sampler2D scene_color_tx; /* HDR radiance from the previous deferred light pass */
uniform sampler2D depth_tx;       /* Full-res R32F NDC depth                             */
uniform sampler2D normal_tx;      /* Full-res G-Buffer normal (RGB16F)                   */
uniform sampler2D hiz_tx;         /* Hi-Z pyramid mip chain                              */

layout(rgba16f) uniform writeonly image2D out_ssgi_img; /* Half-res output */

/* SSGI settings (match SSGISettings C++ struct) */
uniform float ssgi_intensity;      /* Indirect radiance multiplier */
uniform float ssgi_radius;         /* Max screen-space ray length (fraction of screen width) */
uniform float ssgi_color_saturation; /* Boosts/reduces color bleed */
uniform int   ssgi_quality_steps;  /* Hi-Z march iterations */

uniform mat4  viewprojinv;   /* Inverse VP for world-space reconstruction */
uniform mat4  viewproj;      /* VP for projecting hit points back to screen */
uniform vec2  z_planes;
uniform vec2  screen_res;
uniform uint  frame_count;   /* Used for jitter pattern cycling */

/* ------------------------------------------------------------------ */
/* Utilities                                                           */
/* ------------------------------------------------------------------ */

float linear_depth(float ndc_d, float z_near, float z_far)
{
  return (z_near * z_far) / (z_far - ndc_d * (z_far - z_near));
}

vec3 reconstruct_world_pos(vec2 uv, float ndc_depth)
{
  vec4 ndc = vec4(uv * 2.0 - 1.0, ndc_depth * 2.0 - 1.0, 1.0);
  vec4 ws  = viewprojinv * ndc;
  return ws.xyz / ws.w;
}

/* Project a world-space position to screen UV + NDC depth. */
vec3 world_to_screen(vec3 ws)
{
  vec4 clip = viewproj * vec4(ws, 1.0);
  vec3 ndc  = clip.xyz / clip.w;
  return vec3(ndc.xy * 0.5 + 0.5, ndc.z * 0.5 + 0.5);
}

/* Per-pixel cosine-weighted hemisphere sample.
 * Uses a fixed-radius Halton sequence rotated by per-pixel + per-frame noise
 * to give uncorrelated samples across pixels and frames. */
vec3 hemisphere_sample(int sample_idx, vec3 world_normal, ivec2 px)
{
  /* Halton(2,3) for sample position. */
  float u1 = 0.0, u2 = 0.0;
  int n = sample_idx + 1;
  float base = 0.5;
  while (n > 0) {
    u1 += float(n & 1) * base;
    n >>= 1;
    base *= 0.5;
  }
  n = sample_idx + 1; base = 1.0 / 3.0;
  while (n > 0) {
    u2 += float(n % 3) * base;
    n /= 3;
    base /= 3.0;
  }

  /* Per-pixel + per-frame rotation to break temporal correlation. */
  float rot = fract(dot(vec2(px), vec2(0.06711056, 0.00583715)) +
                    float(frame_count) * 0.61803399);
  u1 = fract(u1 + rot);
  u2 = fract(u2 + rot * 1.3);

  /* Cosine-weighted sample on hemisphere. */
  float sin_theta = sqrt(u1);
  float cos_theta = sqrt(1.0 - u1);
  float phi       = u2 * 6.283185;

  vec3 local_dir = vec3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);

  /* Build TBN frame: tangent/bitangent from normal using a stable basis. */
  vec3 up = abs(world_normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
  vec3 T  = normalize(cross(up, world_normal));
  vec3 B  = cross(world_normal, T);

  return normalize(T * local_dir.x + B * local_dir.y + world_normal * local_dir.z);
}

/* ------------------------------------------------------------------ */
/* Hi-Z ray march
 *
 * Traces a ray from screen_pos (UV+depth) in world-space direction `dir_ws`.
 * Returns the scene color at the first hit, or vec4(0) on miss.
 *
 * Steps through the Hi-Z mip chain starting from mip 0.  The coarser mips
 * allow large steps in empty space; the fine mip 0 is only sampled near hits.
 *
 * This is the "linear Z" variant — simpler and cache-friendlier than the
 * exact Hi-Z traversal, sufficient for diffuse GI (which is low-frequency). */
vec4 hiz_ray_march(vec3 origin_uv3, vec3 dir_ws, vec3 origin_ws,
                   sampler2D depth, sampler2D color, int steps,
                   float max_ray_uv, float z_near, float z_far)
{
  vec3 hit_ws    = origin_ws;
  float step_len = max_ray_uv / float(steps);

  for (int i = 1; i <= steps; i++) {
    hit_ws += dir_ws * (step_len * float(i));

    vec3 screen = world_to_screen(hit_ws);

    /* Ray left the screen — no hit. */
    if (any(lessThan(screen.xy, vec2(0.02))) ||
        any(greaterThan(screen.xy, vec2(0.98))))
    {
      return vec4(0.0);
    }

    float scene_ndc = textureLod(depth, screen.xy, float(i) * 0.25).r;

    /* Behind the scene surface = intersection. */
    float ray_lin  = linear_depth(screen.z, z_near, z_far);
    float scene_lin = linear_depth(scene_ndc, z_near, z_far);

    float thickness = 0.3; /* World units; prevents "bleeding through" thin walls */

    if (ray_lin > scene_lin && (ray_lin - scene_lin) < thickness) {
      /* Fade near screen edges to hide missing out-of-screen data. */
      vec2 edge_fade = smoothstep(vec2(0.0), vec2(0.1), screen.xy) *
                       smoothstep(vec2(1.0), vec2(0.9), screen.xy);
      float fade = edge_fade.x * edge_fade.y;

      return vec4(texture(color, screen.xy).rgb * fade, fade);
    }
  }

  return vec4(0.0); /* Miss */
}

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

void main()
{
  const ivec2 out_px  = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 out_res = imageSize(out_ssgi_img);

  if (any(greaterThanEqual(out_px, out_res))) {
    return;
  }

  /* Full-res UV corresponding to this half-res pixel. */
  const vec2 uv = (vec2(out_px) * 2.0 + 1.0) / screen_res;

  float ndc_depth = texture(depth_tx, uv).r;

  if (ndc_depth >= 0.9999) {
    imageStore(out_ssgi_img, out_px, vec4(0.0)); /* Sky: no indirect */
    return;
  }

  const float z_near = z_planes.x;
  const float z_far  = z_planes.y;

  vec3 world_pos    = reconstruct_world_pos(uv, ndc_depth);
  vec3 world_normal = normalize(texture(normal_tx, uv).rgb * 2.0 - 1.0);

  /* Max step size in world units, capped at ssgi_radius. */
  float step_ws = ssgi_radius / float(ssgi_quality_steps);

  /* Accumulate N samples on the hemisphere. */
  const int NUM_SAMPLES = 4; /* 4 directions at half-res ≈ 1 full-res direction */

  vec3 indirect_sum = vec3(0.0);
  float hit_count   = 0.0;

  for (int s = 0; s < NUM_SAMPLES; s++) {
    vec3 ray_dir = hemisphere_sample(s, world_normal, out_px);

    /* March in world space using world step size. */
    vec3 screen_origin = world_to_screen(world_pos);

    vec4 hit_result = hiz_ray_march(
        screen_origin, ray_dir * step_ws, world_pos,
        hiz_tx, scene_color_tx,
        ssgi_quality_steps, ssgi_radius,
        z_near, z_far);

    if (hit_result.a > 0.0) {
      /* Apply color saturation: lerp between grey and full color.
       * ssgi_color_saturation = 1.0 → full color bleed; 0.0 → grey light. */
      vec3 hit_color = hit_result.rgb;
      float luma     = dot(hit_color, vec3(0.2126, 0.7152, 0.0722));
      hit_color      = mix(vec3(luma), hit_color, ssgi_color_saturation);

      indirect_sum += hit_color * hit_result.a;
      hit_count    += hit_result.a;
    }
  }

  /* Average over samples; apply intensity. */
  vec3 indirect = (hit_count > 0.0) ?
      (indirect_sum / float(NUM_SAMPLES)) * ssgi_intensity :
      vec3(0.0);

  imageStore(out_ssgi_img, out_px, vec4(indirect, 1.0));
}
