/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * GTAO — Ground Truth Ambient Occlusion (half-resolution)
 *
 * Algorithm: Horizon-based AO (HBAO variant with cosine weighting).
 *
 * For each pixel, we shoot a fixed number of directions in the tangent plane
 * and for each direction find the maximum elevation angle to the horizon.
 * The AO factor is derived from the solid angle of the occluded hemisphere:
 *
 *   AO = 1 - (1/N) * Σ_directions [sin(h1) + sin(h2)] * 0.5
 *
 * where h1 and h2 are the maximum horizon angles in the positive and negative
 * ray directions.
 *
 * Reference: "Multi-Layer Dual-Resolution Screen-Space Ambient Occlusion"
 *             Jimenez et al. 2016 (GTAO formulation).
 *
 * Implementation choices for real-time:
 *   - 4 directions × quality_steps samples per direction (default: 4×6 = 24 taps)
 *   - Half-res to cut cost by 4×; bilateral upsample restores edge sharpness
 *   - Noise pattern: spatial screen-space hash to vary direction per pixel,
 *     eliminating the banding that fixed-direction sampling produces
 *   - No temporal accumulation here (TAA/FSR provides it for free)
 *
 * Local group 8×8 = 64 threads.
 */

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

uniform sampler2D depth_tx;    /* Full-res R32F hardware depth */
uniform sampler2D normal_tx;   /* G-Buffer normal (RG16F oct-packed or RGB16F) */
uniform sampler2D hiz_tx;      /* Hi-Z pyramid for accelerated ray march */

layout(r8) uniform writeonly image2D out_ao_img;  /* R8 half-res output */

/* GTAO settings (match GTAOSettings C++ struct) */
uniform float gtao_radius;        /* World-space sampling radius */
uniform float gtao_falloff;       /* Exponent on distance attenuation */
uniform float gtao_intensity;     /* Multiplicative boost */
uniform int   gtao_quality_steps; /* Steps per direction */

/* Camera matrices for position reconstruction */
uniform mat4 viewprojinv;   /* Inverse view-projection for world pos rebuild */
uniform vec2 z_planes;      /* x = z_near, y = z_far */
uniform vec2 screen_res;    /* Full-res pixel dimensions */

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

float linear_depth(float ndc_d, float z_near, float z_far)
{
  return (z_near * z_far) / (z_far - ndc_d * (z_far - z_near));
}

/* Reconstruct world-space position from UV and NDC depth. */
vec3 reconstruct_world_pos(vec2 uv, float ndc_depth)
{
  vec4 ndc = vec4(uv * 2.0 - 1.0, ndc_depth * 2.0 - 1.0, 1.0);
  vec4 ws  = viewprojinv * ndc;
  return ws.xyz / ws.w;
}

/* Low-discrepancy per-pixel noise angle to avoid directional banding.
 * Uses a spatial hash so we get different directions per pixel without
 * storing a texture or using a PRNG (both are slower for half-res compute). */
float noise_angle(ivec2 px)
{
  /* Interleaved gradient noise (Jimenez 2014): fast, good spatial distribution. */
  float magic = dot(vec2(px), vec2(0.06711056, 0.00583715));
  return fract(52.9829189 * fract(magic)) * 6.283185; /* [0, 2π) */
}

/* ------------------------------------------------------------------ */
/* Single direction horizon search
 *
 * Steps along `dir` in view space, sampling the Hi-Z to find the
 * maximum angle above the tangent plane of the surface normal.
 *
 * Returns the horizon angle (radians) in [0, π/2]. */
float horizon_search(vec3 world_pos, vec3 world_normal, vec2 base_uv, vec2 ray_uv_dir,
                     float step_size_uv, int steps, sampler2D depth,
                     float z_near, float z_far)
{
  float max_horizon = -1.0;  /* sin(angle) */

  /* base_uv is the correctly projected UV of the surface point, computed in
   * main() from the half-res pixel coordinate and screen_res. Marching from
   * this UV in screen space is correct; the old approach divided world-space
   * metres by pixel dimensions, producing UV values that scale with scene size. */
  vec2 uv = base_uv;

  for (int i = 1; i <= steps; i++) {
    /* March along the direction in screen UV space. */
    vec2 sample_uv = uv + ray_uv_dir * (float(i) * step_size_uv);

    /* Clamp to [0,1] — don't wrap around screen edges. */
    if (any(lessThan(sample_uv, vec2(0.0))) ||
        any(greaterThan(sample_uv, vec2(1.0))))
    {
      break;
    }

    /* Sample Hi-Z mip corresponding to step distance (coarser mip = cheaper = same quality
     * because near steps are oversampled relative to the solid angle they subtend). */
    float mip        = log2(float(i)) * 0.5;
    float ndc_depth  = textureLod(depth, sample_uv, mip).r;

    if (ndc_depth >= 0.9999) {
      continue;  /* Sky — no occlusion. */
    }

    float lin_depth  = linear_depth(ndc_depth, z_near, z_far);
    vec3  sample_pos = reconstruct_world_pos(sample_uv, ndc_depth);
    vec3  horizon_v  = sample_pos - world_pos;
    float len        = length(horizon_v);

    if (len > gtao_radius) {
      break;  /* Past influence radius — further steps can only be farther. */
    }

    /* Attenuation: falls off with distance so nearby geometry dominates. */
    float falloff = 1.0 - pow(len / gtao_radius, gtao_falloff);

    /* sin(elevation) = dot(horizon_dir, normal) / len */
    float sin_h = dot(horizon_v, world_normal) / max(len, 1e-4) * falloff;

    max_horizon = max(max_horizon, sin_h);
  }

  return max(max_horizon, 0.0);
}

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

void main()
{
  /* This pass runs at half resolution; output px is in the half-res domain. */
  const ivec2 out_px  = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 out_res = imageSize(out_ao_img);

  if (any(greaterThanEqual(out_px, out_res))) {
    return;
  }

  /* Sample the full-res depth at the centre of the 2×2 block this pixel covers. */
  const vec2 uv = (vec2(out_px) * 2.0 + 1.0) / screen_res;

  float ndc_depth = texture(depth_tx, uv).r;

  /* Sky: no occlusion, write 1.0 (fully lit) immediately. */
  if (ndc_depth >= 0.9999) {
    imageStore(out_ao_img, out_px, vec4(1.0));
    return;
  }

  const float z_near = z_planes.x;
  const float z_far  = z_planes.y;

  vec3 world_pos    = reconstruct_world_pos(uv, ndc_depth);
  vec3 world_normal = normalize(texture(normal_tx, uv).rgb * 2.0 - 1.0);

  /* Step size in UV: covers gtao_radius in screen space.
   * We limit to 1/8 of the screen to avoid over-large steps that miss geometry. */
  float step_size_uv = clamp(gtao_radius / (float(gtao_quality_steps) * screen_res.x),
                             1.0 / screen_res.x, 0.125);

  /* 4 evenly-spaced directions with per-pixel noise rotation. */
  const int NUM_DIRS = 4;
  float noise_rot = noise_angle(out_px);

  float ao_accum = 0.0;

  for (int d = 0; d < NUM_DIRS; d++) {
    float angle = noise_rot + float(d) * (3.14159265 / float(NUM_DIRS));
    vec2  dir   = vec2(cos(angle), sin(angle));

    /* Horizon search in both ±directions along this axis. */
    /* Pass the half-res UV (== full-res UV of the 2x2 block centre) so
     * horizon_search marches in correct screen space from the right origin. */
    float h_pos = horizon_search(world_pos, world_normal, uv,  dir,
                                 step_size_uv, gtao_quality_steps,
                                 hiz_tx, z_near, z_far);
    float h_neg = horizon_search(world_pos, world_normal, uv, -dir,
                                 step_size_uv, gtao_quality_steps,
                                 hiz_tx, z_near, z_far);

    /* Cosine-weighted hemisphere integral approximation:
     *   AO_dir = 1 - (sin(h_pos) + sin(h_neg)) * 0.5
     * Where h_pos/neg are already in sin-space from horizon_search. */
    ao_accum += 1.0 - (h_pos + h_neg) * 0.5;
  }

  float ao = ao_accum / float(NUM_DIRS);

  /* Intensity multiplier: clamp to avoid negative values from noisy normals. */
  ao = clamp(ao * gtao_intensity, 0.0, 1.0);

  imageStore(out_ao_img, out_px, vec4(ao, 0.0, 0.0, 0.0));
}
