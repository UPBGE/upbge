/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Volumetric Resolve — Froxel-to-Screen Composite
 *
 * Pass 3 (final) of the volumetric pipeline.  This is a 2D full-screen compute
 * shader — one thread per output pixel.
 *
 * Algorithm:
 *   1. Read hardware depth to get per-pixel view depth.
 *   2. Convert pixel + depth → froxel UVW in [0,1]^3.
 *   3. Sample the integrated 3D texture (volume_integrated_tx):
 *        RGB = accumulated in-scattered radiance L(z_pixel)
 *        A   = accumulated transmittance T(z_pixel)
 *   4. Composite:
 *        out_color = scene_color × T + L_inscatter
 *
 * The Beer-Lambert composite is physically correct for a homogeneous medium:
 *   The scene color is attenuated by the transmittance (fog occlusion),
 *   and the in-scattered radiance is added (fog glow / godrays).
 *
 * UVW derivation:
 *   UV  = screen position  → direct from pixel
 *   W   = log-remapped depth slice index
 *         w = log(z_lin / z_near) / log(z_far / z_near)
 *   This exactly inverts the exponential Z spacing used in the scatter pass.
 *
 * Dispatch: ceil(render_res.x / 8) × ceil(render_res.y / 8) × 1.
 *
 * Local group 8×8×1.
 */

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

/* Integrated froxel radiance + transmittance from the integration pass. */
uniform sampler3D volume_integrated_tx;

/* Full-res hardware depth for per-pixel froxel UVW reconstruction. */
uniform sampler2D depth_tx;

/* Scene color (HDR, RGBA16F) — modified in-place (READ_WRITE). */
layout(rgba16f) uniform image2D out_color_img;

/* Camera clip planes and froxel grid parameters. */
uniform float z_near;
uniform float z_far;
uniform int   tile_size;   /* Screen pixels per froxel XY cell */
uniform int   samples_z;   /* Froxel depth slices */
uniform vec2  screen_res;  /* Full render resolution */

/* ------------------------------------------------------------------ */
/* Linear depth from NDC depth.
 * Maps OpenGL NDC [0,1] depth to view-space [z_near, z_far]. */
float linear_depth(float ndc_d)
{
  return (z_near * z_far) / (z_far - ndc_d * (z_far - z_near));
}

/* Map linear view depth to the froxel W coordinate [0,1].
 * Exact inverse of the exponential spacing used in the scatter and
 * integration passes:
 *   z(w) = z_near × (z_far/z_near)^w
 *   → w  = log(z_lin/z_near) / log(z_far/z_near)
 *
 * Clamp to [0,1]: pixels outside the froxel z-range (closer than z_near
 * or farther than z_far of the volume grid) get the nearest boundary value. */
float depth_to_froxel_w(float z_lin)
{
  float log_ratio = log(z_far / z_near);
  float w = log(max(z_lin, z_near) / z_near) / log_ratio;
  return clamp(w, 0.0, 1.0);
}

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

void main()
{
  const ivec2 px  = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 res = imageSize(out_color_img);

  if (any(greaterThanEqual(px, res))) {
    return;
  }

  const vec2 uv = (vec2(px) + 0.5) / screen_res;

  /* Read NDC depth, convert to linear. */
  float ndc_depth = texture(depth_tx, uv).r;

  /* Sky pixels (far plane) receive full volume transmittance from the last
   * froxel slice.  Sample at w = 1.0 to get the fog-to-infinity value. */
  float z_lin = (ndc_depth >= 0.9999) ?
      z_far :
      linear_depth(ndc_depth);

  /* Build 3D UVW for the volume texture lookup.
   * UV = screen UV (froxel XY tiles cover the full screen).
   * W  = log-remapped depth slice [0, 1]. */
  float froxel_w = depth_to_froxel_w(z_lin);
  vec3  uvw      = vec3(uv, froxel_w);

  /* Sample the integrated volume.  Trilinear interpolation (default) gives
   * smooth depth transitions between froxel slices at no extra ALU cost. */
  vec4 volume = texture(volume_integrated_tx, uvw);
  vec3 L_scatter    = volume.rgb;  /* Accumulated in-scattered radiance */
  float transmittance = volume.a;  /* Accumulated transmittance [0,1] */

  /* Read current scene color for this pixel. */
  vec4 scene = imageLoad(out_color_img, px);

  /* Beer-Lambert composite:
   *   out = scene × T + L_scatter
   * T = 1 → clear air, scene unchanged.
   * T = 0 → full fog, only in-scattered light visible. */
  vec3 composited = scene.rgb * transmittance + L_scatter;

  imageStore(out_color_img, px, vec4(composited, scene.a));
}
