/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Visibility of each resource bounds for the main camera view.
 * Frustum test, then Hi-Z occlusion test against the previous frame.
 *
 * Drop-in replacement for `draw_visibility_comp.glsl` (single view only):
 * same `bounds_buf` input, same `visibility_buf` bitmask output.
 */

#include "infos/eevee_game_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_game_culling_compute)

#include "draw_intersect_lib.glsl"

/* Largest footprint, in texels of the coarsest mip, that is still tested.
 * A box of N pixels lands on mip ceil(log2(N)), where it touches at most 2x2
 * texels. Only boxes bigger than one texel of the last mip (64 px) need more,
 * and those are rarely fully hidden, so the loop is capped instead of adding
 * mips to the pyramid. 4x4 covers footprints up to 256 px. */
#define HIZ_CULL_MAX_SPAN 4

/* The box depth comes from 8 matrix products here, the stored depth from the
 * rasterizer interpolating the same surface. A camera facing quad that fills
 * its own bounds (sprite, decal, wall) gives two values that differ only by
 * rounding, and without slack it would cull itself on alternate frames.
 * A few float32 ulps near 1.0, where perspective depth is the least precise. */
#define HIZ_CULL_DEPTH_BIAS 1e-6f

void mask_visibility_bit()
{
  /* One word per work group: 32 threads, 32 bits. */
  atomicAnd(visibility_buf[gl_WorkGroupID.x], ~(1u << gl_LocalInvocationID.x));
}

bool hiz_occluded(IsectBox box)
{
  float2 ndc_min = float2(1e30f);
  float2 ndc_max = float2(-1e30f);
  float ndc_z_min = 1e30f;

  for (int i = 0; i < 8; i++) {
    float4 hs = hiz_persmat * float4(box.corners[i], 1.0f);
    if (hs.w <= 0.0f) {
      /* Behind the previous camera: the projection is not a rectangle. */
      return false;
    }
    float3 ndc = hs.xyz / hs.w;
    ndc_min = min(ndc_min, ndc.xy);
    ndc_max = max(ndc_max, ndc.xy);
    ndc_z_min = min(ndc_z_min, ndc.z);
  }

  /* Any part outside the previous frame has no depth to compare against.
   * This also keeps objects entering the screen from popping in late. */
  if (any(lessThan(ndc_min, float2(-1.0f))) || any(greaterThan(ndc_max, float2(1.0f))) ||
      ndc_z_min < -1.0f || ndc_z_min > 1.0f)
  {
    return false;
  }

  /* The pyramid stores non reversed depth (0 = near, 1 = far) and each texel
   * holds the farthest value it covers. Matrices keep the GL convention. */
  float box_depth = ndc_z_min * 0.5f + 0.5f;

  /* The texture is padded to a multiple of 64; only this part holds depth. */
  float2 render_extent = float2(textureSize(hiz_tx, 0)) * hiz_uv_scale;
  float2 px_max = min((ndc_max * 0.5f + 0.5f) * render_extent, render_extent - 1.0f);
  float2 px_min = min((ndc_min * 0.5f + 0.5f) * render_extent, px_max);
  float2 px_size = px_max - px_min;

  int mip = int(ceil(log2(max(max(px_size.x, px_size.y), 1.0f))));
  mip = clamp(mip, 0, HIZ_MIP_COUNT - 1);

  int2 texel_min = int2(px_min) >> mip;
  int2 texel_max = int2(px_max) >> mip;
  if (any(greaterThan(texel_max - texel_min, int2(HIZ_CULL_MAX_SPAN - 1)))) {
    return false;
  }

  float occluder_depth = 0.0f;
  for (int y = texel_min.y; y <= texel_max.y; y++) {
    for (int x = texel_min.x; x <= texel_max.x; x++) {
      occluder_depth = max(occluder_depth, texelFetch(hiz_tx, int2(x, y), mip).r);
    }
  }

  /* Hidden only if the nearest point of the box is behind the farthest
   * occluder under its whole footprint. */
  return box_depth > occluder_depth + HIZ_CULL_DEPTH_BIAS;
}

void main()
{
  if (int(gl_GlobalInvocationID.x) >= resource_len) {
    return;
  }

  ObjectBounds bounds = bounds_buf[gl_GlobalInvocationID.x];

  if (!drw_bounds_are_valid(bounds)) {
    /* Culling is disabled for this resource. */
    return;
  }

  IsectBox box = isect_box_setup(bounds.bounding_corners[0].xyz,
                                 bounds.bounding_corners[1].xyz,
                                 bounds.bounding_corners[2].xyz,
                                 bounds.bounding_corners[3].xyz);
  Sphere bounding_sphere = shape_sphere(bounds.bounding_sphere.xyz, bounds.bounding_sphere.w);
  Sphere inscribed_sphere = shape_sphere(bounds.bounding_sphere.xyz, bounds._inner_sphere_radius);

  /* Frustum: same cheap to expensive cascade as the stock shader. */
  if (intersect_view(inscribed_sphere, 0u) == false) {
    if (intersect_view(bounding_sphere, 0u) == false || intersect_view(box, 0u) == false) {
      mask_visibility_bit();
      return;
    }
  }

  if (use_occlusion && hiz_occluded(box)) {
    mask_visibility_bit();
  }
}
