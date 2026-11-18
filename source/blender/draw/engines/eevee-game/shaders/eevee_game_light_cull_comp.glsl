/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Tiled light culling.
 *
 * One thread per (screen tile, word of 32 lights). The tile volume is the
 * frustum of its 16x16 pixels, from the near plane to the farthest opaque
 * depth of the tile (one Hi-Z mip 4 fetch): nothing behind it can be seen,
 * neither through the opaque surfaces nor through the transparent ones that
 * are drawn in front of them. Lights are bounded by a sphere (a cone bound
 * for spots); suns reach every tile.
 *
 * Per pixel the lighting then walks only the set bits of its tile.
 */

#include "infos/eevee_game_infos.hh"

COMPUTE_SHADER_CREATE_INFO(eevee_game_light_cull)

#include "draw_view_lib.glsl"

/* View space point of a screen position: NDC xy, conventional depth. */
float3 view_point(float2 ndc_xy, float depth)
{
  float4 p = drw_view().wininv * float4(ndc_xy, depth * 2.0f - 1.0f, 1.0f);
  return p.xyz / p.w;
}

/* Plane through a, b, c (xyz normal, w offset) with `inside` on its
 * positive side. */
float4 plane_from_points(float3 a, float3 b, float3 c, float3 inside)
{
  float3 n = normalize(cross(b - a, c - a));
  float4 plane = float4(n, -dot(n, a));
  return (dot(plane.xyz, inside) + plane.w < 0.0f) ? -plane : plane;
}

/* Bounding sphere of what a light can reach, in view space. */
float4 light_bound_view(GameLightData light)
{
  float3 center = light.position;
  /* The emitter size widens the reach of area and soft lights. */
  float radius = light.attenuation + light.radius;
  if (light.type == LIGHT_TYPE_SPOT) {
    /* Circumsphere of the cone of length `attenuation` and half angle h,
     * the same h as the shading: x = 1 - cos(h). Past 90 degrees (x > 1)
     * the hemisphere bound is kept. */
    float x = min(unpackHalf2x16(light.spot_shape).x, 1.0f);
    float c = 1.0f - x;
    if (c >= 0.7071068f) {
      float r = light.attenuation / (2.0f * c);
      center += light.direction * r;
      radius = r + light.radius;
    }
    else {
      center += light.direction * (light.attenuation * c);
      radius = light.attenuation * sqrt(x * (2.0f - x)) + light.radius;
    }
  }
  return float4((drw_view().viewmat * float4(center, 1.0f)).xyz, radius);
}

void main()
{
  int gid = int(gl_GlobalInvocationID.x);
  int word_count = game_lighting_buf.word_count;
  int2 tile_count = game_lighting_buf.tile_count;
  int tile_index = gid / word_count;
  int word = gid - tile_index * word_count;
  if (tile_index >= tile_count.x * tile_count.y) {
    return;
  }
  int2 tile = int2(tile_index % tile_count.x, tile_index / tile_count.x);

  /* Tile rectangle in NDC. */
  float2 extent = float2(game_lighting_buf.render_extent);
  float2 px0 = float2(tile * LIGHT_TILE_SIZE);
  float2 px1 = min(px0 + float(LIGHT_TILE_SIZE), extent);
  float2 ndc0 = px0 / extent * 2.0f - 1.0f;
  float2 ndc1 = px1 / extent * 2.0f - 1.0f;

  /* Kept off the near plane: a zero depth slab would make the side planes
   * degenerate. */
  float far_depth = max(texelFetch(hiz_tx, tile, LIGHT_TILE_HIZ_LOD).r, 1e-3f);

  /* Corners: near (depth 0) and far (farthest opaque depth). */
  float3 n00 = view_point(float2(ndc0.x, ndc0.y), 0.0f);
  float3 n10 = view_point(float2(ndc1.x, ndc0.y), 0.0f);
  float3 n01 = view_point(float2(ndc0.x, ndc1.y), 0.0f);
  float3 n11 = view_point(float2(ndc1.x, ndc1.y), 0.0f);
  float3 f00 = view_point(float2(ndc0.x, ndc0.y), far_depth);
  float3 f11 = view_point(float2(ndc1.x, ndc1.y), far_depth);
  float3 inside = view_point((ndc0 + ndc1) * 0.5f, far_depth * 0.5f);

  /* Side planes from three corners each: valid for perspective and
   * orthographic projections alike. */
  float4 planes[4];
  planes[0] = plane_from_points(n00, n01, f00, inside); /* Left. */
  planes[1] = plane_from_points(n10, n11, f11, inside); /* Right. */
  planes[2] = plane_from_points(n00, n10, f00, inside); /* Bottom. */
  planes[3] = plane_from_points(n01, n11, f11, inside); /* Top. */
  /* The view looks down -Z. */
  float z_near = n00.z;
  float z_far = f00.z;

  uint mask = 0u;
  int first = word * 32;
  int last = min(first + 32, game_lighting_buf.light_count);
  for (int l = first; l < last; l++) {
    GameLightData light = light_buf[l];
    bool visible = true;
    if (light.type != LIGHT_TYPE_SUN) {
      float4 s = light_bound_view(light);
      visible = (s.z + s.w >= z_far) && (s.z - s.w <= z_near);
      for (int p = 0; p < 4 && visible; p++) {
        visible = dot(planes[p].xyz, s.xyz) + planes[p].w >= -s.w;
      }
    }
    if (visible) {
      mask |= 1u << uint(l - first);
    }
  }
  light_tile_mask_out_buf[tile_index * word_count + word] = mask;
}
