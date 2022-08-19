/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#pragma once

CCL_NAMESPACE_BEGIN

ccl_device float4 svm_image_texture(KernelGlobals kg, int id, float x, float y, uint flags)
{
  if (id == -1) {
    return make_float4(
        TEX_IMAGE_MISSING_R, TEX_IMAGE_MISSING_G, TEX_IMAGE_MISSING_B, TEX_IMAGE_MISSING_A);
  }

  float4 r = kernel_tex_image_interp(kg, id, x, y);
  const float alpha = r.w;

  if ((flags & NODE_IMAGE_ALPHA_UNASSOCIATE) && alpha != 1.0f && alpha != 0.0f) {
    r /= alpha;
    r.w = alpha;
  }

  if (flags & NODE_IMAGE_COMPRESS_AS_SRGB) {
    r = color_srgb_to_linear_v4(r);
  }

  return r;
}

/* Remap coordinate from 0..1 box to -1..-1 */
ccl_device_inline float3 texco_remap_square(float3 co)
{
  return (co - make_float3(0.5f, 0.5f, 0.5f)) * 2.0f;
}

ccl_device_noinline int svm_node_tex_image(
    KernelGlobals kg, ccl_private ShaderData *sd, ccl_private float *stack, uint4 node, int offset)
{
  uint co_offset, out_offset, alpha_offset, flags;

  svm_unpack_node_uchar4(node.z, &co_offset, &out_offset, &alpha_offset, &flags);

  float3 co = stack_load_float3(stack, co_offset);
  float2 tex_co;
  if (node.w == NODE_IMAGE_PROJ_SPHERE) {
    co = texco_remap_square(co);
    tex_co = map_to_sphere(co);
  }
  else if (node.w == NODE_IMAGE_PROJ_TUBE) {
    co = texco_remap_square(co);
    tex_co = map_to_tube(co);
  }
  else {
    tex_co = make_float2(co.x, co.y);
  }

  /* TODO(lukas): Consider moving tile information out of the SVM node.
   * TextureInfo seems a reasonable candidate. */
  int id = -1;
  int num_nodes = (int)node.y;
  if (num_nodes > 0) {
    /* Remember the offset of the node following the tile nodes. */
    int next_offset = offset + num_nodes;

    /* Find the tile that the UV lies in. */
    int tx = (int)tex_co.x;
    int ty = (int)tex_co.y;

    /* Check that we're within a legitimate tile. */
    if (tx >= 0 && ty >= 0 && tx < 10) {
      int tile = 1001 + 10 * ty + tx;

      /* Find the index of the tile. */
      for (int i = 0; i < num_nodes; i++) {
        uint4 tile_node = read_node(kg, &offset);
        if (tile_node.x == tile) {
          id = tile_node.y;
          break;
        }
        if (tile_node.z == tile) {
          id = tile_node.w;
          break;
        }
      }

      /* If we found the tile, offset the UVs to be relative to it. */
      if (id != -1) {
        tex_co.x -= tx;
        tex_co.y -= ty;
      }
    }

    /* Skip over the remaining nodes. */
    offset = next_offset;
  }
  else {
    id = -num_nodes;
  }

  float4 f = svm_image_texture(kg, id, tex_co.x, tex_co.y, flags);

  if (stack_valid(out_offset))
    stack_store_float3(stack, out_offset, make_float3(f.x, f.y, f.z));
  if (stack_valid(alpha_offset))
    stack_store_float(stack, alpha_offset, f.w);
  return offset;
}

ccl_device_noinline void svm_node_tex_image_box(KernelGlobals kg,
                                                ccl_private ShaderData *sd,
                                                ccl_private float *stack,
                                                uint4 node)
{
  /* get object space normal */
  float3 N = sd->N;

  N = sd->N;
  object_inverse_normal_transform(kg, sd, &N);

  /* project from direction vector to barycentric coordinates in triangles */
  float3 signed_N = N;

  N.x = fabsf(N.x);
  N.y = fabsf(N.y);
  N.z = fabsf(N.z);

  N /= (N.x + N.y + N.z);

  /* basic idea is to think of this as a triangle, each corner representing
   * one of the 3 faces of the cube. in the corners we have single textures,
   * in between we blend between two textures, and in the middle we a blend
   * between three textures.
   *
   * The `Nxyz` values are the barycentric coordinates in an equilateral
   * triangle, which in case of blending, in the middle has a smaller
   * equilateral triangle where 3 textures blend. this divides things into
   * 7 zones, with an if() test for each zone. */

  float3 weight = make_float3(0.0f, 0.0f, 0.0f);
  float blend = __int_as_float(node.w);
  float limit = 0.5f * (1.0f + blend);

  /* first test for corners with single texture */
  if (N.x > limit * (N.x + N.y) && N.x > limit * (N.x + N.z)) {
    weight.x = 1.0f;
  }
  else if (N.y > limit * (N.x + N.y) && N.y > limit * (N.y + N.z)) {
    weight.y = 1.0f;
  }
  else if (N.z > limit * (N.x + N.z) && N.z > limit * (N.y + N.z)) {
    weight.z = 1.0f;
  }
  else if (blend > 0.0f) {
    /* in case of blending, test for mixes between two textures */
    if (N.z < (1.0f - limit) * (N.y + N.x)) {
      weight.x = N.x / (N.x + N.y);
      weight.x = saturatef((weight.x - 0.5f * (1.0f - blend)) / blend);
      weight.y = 1.0f - weight.x;
    }
    else if (N.x < (1.0f - limit) * (N.y + N.z)) {
      weight.y = N.y / (N.y + N.z);
      weight.y = saturatef((weight.y - 0.5f * (1.0f - blend)) / blend);
      weight.z = 1.0f - weight.y;
    }
    else if (N.y < (1.0f - limit) * (N.x + N.z)) {
      weight.x = N.x / (N.x + N.z);
      weight.x = saturatef((weight.x - 0.5f * (1.0f - blend)) / blend);
      weight.z = 1.0f - weight.x;
    }
    else {
      /* last case, we have a mix between three */
      weight.x = ((2.0f - limit) * N.x + (limit - 1.0f)) / (2.0f * limit - 1.0f);
      weight.y = ((2.0f - limit) * N.y + (limit - 1.0f)) / (2.0f * limit - 1.0f);
      weight.z = ((2.0f - limit) * N.z + (limit - 1.0f)) / (2.0f * limit - 1.0f);
    }
  }
  else {
    /* Desperate mode, no valid choice anyway, fallback to one side. */
    weight.x = 1.0f;
  }

  /* now fetch textures */
  uint co_offset, out_offset, alpha_offset, flags;
  svm_unpack_node_uchar4(node.z, &co_offset, &out_offset, &alpha_offset, &flags);

  float3 co = stack_load_float3(stack, co_offset);
  uint id = node.y;

  float4 f = zero_float4();

  /* Map so that no textures are flipped, rotation is somewhat arbitrary. */
  if (weight.x > 0.0f) {
    float2 uv = make_float2((signed_N.x < 0.0f) ? 1.0f - co.y : co.y, co.z);
    f += weight.x * svm_image_texture(kg, id, uv.x, uv.y, flags);
  }
  if (weight.y > 0.0f) {
    float2 uv = make_float2((signed_N.y > 0.0f) ? 1.0f - co.x : co.x, co.z);
    f += weight.y * svm_image_texture(kg, id, uv.x, uv.y, flags);
  }
  if (weight.z > 0.0f) {
    float2 uv = make_float2((signed_N.z > 0.0f) ? 1.0f - co.y : co.y, co.x);
    f += weight.z * svm_image_texture(kg, id, uv.x, uv.y, flags);
  }

  if (stack_valid(out_offset))
    stack_store_float3(stack, out_offset, make_float3(f.x, f.y, f.z));
  if (stack_valid(alpha_offset))
    stack_store_float(stack, alpha_offset, f.w);
}

ccl_device_noinline void svm_node_tex_environment(KernelGlobals kg,
                                                  ccl_private ShaderData *sd,
                                                  ccl_private float *stack,
                                                  uint4 node)
{
  uint id = node.y;
  uint co_offset, out_offset, alpha_offset, flags;
  uint projection = node.w;

  svm_unpack_node_uchar4(node.z, &co_offset, &out_offset, &alpha_offset, &flags);

  float3 co = stack_load_float3(stack, co_offset);
  float2 uv;

  co = safe_normalize(co);

  if (projection == 0)
    uv = direction_to_equirectangular(co);
  else
    uv = direction_to_mirrorball(co);

  float4 f = svm_image_texture(kg, id, uv.x, uv.y, flags);

  if (stack_valid(out_offset))
    stack_store_float3(stack, out_offset, make_float3(f.x, f.y, f.z));
  if (stack_valid(alpha_offset))
    stack_store_float(stack, alpha_offset, f.w);
}

CCL_NAMESPACE_END
