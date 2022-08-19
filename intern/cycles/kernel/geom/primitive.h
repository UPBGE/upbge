/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

/* Primitive Utilities
 *
 * Generic functions to look up mesh, curve and volume primitive attributes for
 * shading and render passes. */

#pragma once

#include "kernel/camera/projection.h"

CCL_NAMESPACE_BEGIN

/* Surface Attributes
 *
 * Read geometry attributes for surface shading. This is distinct from volume
 * attributes for performance, mainly for GPU performance to avoid bringing in
 * heavy volume interpolation code. */

ccl_device_forceinline float primitive_surface_attribute_float(KernelGlobals kg,
                                                               ccl_private const ShaderData *sd,
                                                               const AttributeDescriptor desc,
                                                               ccl_private float *dx,
                                                               ccl_private float *dy)
{
  if (sd->type & PRIMITIVE_TRIANGLE) {
    if (subd_triangle_patch(kg, sd) == ~0)
      return triangle_attribute_float(kg, sd, desc, dx, dy);
    else
      return subd_triangle_attribute_float(kg, sd, desc, dx, dy);
  }
#ifdef __HAIR__
  else if (sd->type & PRIMITIVE_CURVE) {
    return curve_attribute_float(kg, sd, desc, dx, dy);
  }
#endif
#ifdef __POINTCLOUD__
  else if (sd->type & PRIMITIVE_POINT) {
    return point_attribute_float(kg, sd, desc, dx, dy);
  }
#endif
  else {
    if (dx)
      *dx = 0.0f;
    if (dy)
      *dy = 0.0f;
    return 0.0f;
  }
}

ccl_device_forceinline float2 primitive_surface_attribute_float2(KernelGlobals kg,
                                                                 ccl_private const ShaderData *sd,
                                                                 const AttributeDescriptor desc,
                                                                 ccl_private float2 *dx,
                                                                 ccl_private float2 *dy)
{
  if (sd->type & PRIMITIVE_TRIANGLE) {
    if (subd_triangle_patch(kg, sd) == ~0)
      return triangle_attribute_float2(kg, sd, desc, dx, dy);
    else
      return subd_triangle_attribute_float2(kg, sd, desc, dx, dy);
  }
#ifdef __HAIR__
  else if (sd->type & PRIMITIVE_CURVE) {
    return curve_attribute_float2(kg, sd, desc, dx, dy);
  }
#endif
#ifdef __POINTCLOUD__
  else if (sd->type & PRIMITIVE_POINT) {
    return point_attribute_float2(kg, sd, desc, dx, dy);
  }
#endif
  else {
    if (dx)
      *dx = make_float2(0.0f, 0.0f);
    if (dy)
      *dy = make_float2(0.0f, 0.0f);
    return make_float2(0.0f, 0.0f);
  }
}

ccl_device_forceinline float3 primitive_surface_attribute_float3(KernelGlobals kg,
                                                                 ccl_private const ShaderData *sd,
                                                                 const AttributeDescriptor desc,
                                                                 ccl_private float3 *dx,
                                                                 ccl_private float3 *dy)
{
  if (sd->type & PRIMITIVE_TRIANGLE) {
    if (subd_triangle_patch(kg, sd) == ~0)
      return triangle_attribute_float3(kg, sd, desc, dx, dy);
    else
      return subd_triangle_attribute_float3(kg, sd, desc, dx, dy);
  }
#ifdef __HAIR__
  else if (sd->type & PRIMITIVE_CURVE) {
    return curve_attribute_float3(kg, sd, desc, dx, dy);
  }
#endif
#ifdef __POINTCLOUD__
  else if (sd->type & PRIMITIVE_POINT) {
    return point_attribute_float3(kg, sd, desc, dx, dy);
  }
#endif
  else {
    if (dx)
      *dx = make_float3(0.0f, 0.0f, 0.0f);
    if (dy)
      *dy = make_float3(0.0f, 0.0f, 0.0f);
    return make_float3(0.0f, 0.0f, 0.0f);
  }
}

ccl_device_forceinline float4 primitive_surface_attribute_float4(KernelGlobals kg,
                                                                 ccl_private const ShaderData *sd,
                                                                 const AttributeDescriptor desc,
                                                                 ccl_private float4 *dx,
                                                                 ccl_private float4 *dy)
{
  if (sd->type & PRIMITIVE_TRIANGLE) {
    if (subd_triangle_patch(kg, sd) == ~0)
      return triangle_attribute_float4(kg, sd, desc, dx, dy);
    else
      return subd_triangle_attribute_float4(kg, sd, desc, dx, dy);
  }
#ifdef __HAIR__
  else if (sd->type & PRIMITIVE_CURVE) {
    return curve_attribute_float4(kg, sd, desc, dx, dy);
  }
#endif
#ifdef __POINTCLOUD__
  else if (sd->type & PRIMITIVE_POINT) {
    return point_attribute_float4(kg, sd, desc, dx, dy);
  }
#endif
  else {
    if (dx)
      *dx = zero_float4();
    if (dy)
      *dy = zero_float4();
    return zero_float4();
  }
}

#ifdef __VOLUME__
/* Volume Attributes
 *
 * Read geometry attributes for volume shading. This is distinct from surface
 * attributes for performance, mainly for GPU performance to avoid bringing in
 * heavy volume interpolation code. */

ccl_device_forceinline bool primitive_is_volume_attribute(ccl_private const ShaderData *sd,
                                                          const AttributeDescriptor desc)
{
  return sd->type == PRIMITIVE_VOLUME;
}

ccl_device_forceinline float primitive_volume_attribute_float(KernelGlobals kg,
                                                              ccl_private const ShaderData *sd,
                                                              const AttributeDescriptor desc)
{
  if (primitive_is_volume_attribute(sd, desc)) {
    return volume_attribute_value_to_float(volume_attribute_float4(kg, sd, desc));
  }
  else {
    return 0.0f;
  }
}

ccl_device_forceinline float3 primitive_volume_attribute_float3(KernelGlobals kg,
                                                                ccl_private const ShaderData *sd,
                                                                const AttributeDescriptor desc)
{
  if (primitive_is_volume_attribute(sd, desc)) {
    return volume_attribute_value_to_float3(volume_attribute_float4(kg, sd, desc));
  }
  else {
    return make_float3(0.0f, 0.0f, 0.0f);
  }
}

ccl_device_forceinline float4 primitive_volume_attribute_float4(KernelGlobals kg,
                                                                ccl_private const ShaderData *sd,
                                                                const AttributeDescriptor desc)
{
  if (primitive_is_volume_attribute(sd, desc)) {
    return volume_attribute_float4(kg, sd, desc);
  }
  else {
    return zero_float4();
  }
}
#endif

/* Default UV coordinate */

ccl_device_forceinline float3 primitive_uv(KernelGlobals kg, ccl_private const ShaderData *sd)
{
  const AttributeDescriptor desc = find_attribute(kg, sd, ATTR_STD_UV);

  if (desc.offset == ATTR_STD_NOT_FOUND)
    return make_float3(0.0f, 0.0f, 0.0f);

  float2 uv = primitive_surface_attribute_float2(kg, sd, desc, NULL, NULL);
  return make_float3(uv.x, uv.y, 1.0f);
}

/* Ptex coordinates */

ccl_device bool primitive_ptex(KernelGlobals kg,
                               ccl_private ShaderData *sd,
                               ccl_private float2 *uv,
                               ccl_private int *face_id)
{
  /* storing ptex data as attributes is not memory efficient but simple for tests */
  const AttributeDescriptor desc_face_id = find_attribute(kg, sd, ATTR_STD_PTEX_FACE_ID);
  const AttributeDescriptor desc_uv = find_attribute(kg, sd, ATTR_STD_PTEX_UV);

  if (desc_face_id.offset == ATTR_STD_NOT_FOUND || desc_uv.offset == ATTR_STD_NOT_FOUND)
    return false;

  float3 uv3 = primitive_surface_attribute_float3(kg, sd, desc_uv, NULL, NULL);
  float face_id_f = primitive_surface_attribute_float(kg, sd, desc_face_id, NULL, NULL);

  *uv = make_float2(uv3.x, uv3.y);
  *face_id = (int)face_id_f;

  return true;
}

/* Surface tangent */

ccl_device float3 primitive_tangent(KernelGlobals kg, ccl_private ShaderData *sd)
{
#if defined(__HAIR__) || defined(__POINTCLOUD__)
  if (sd->type & (PRIMITIVE_CURVE | PRIMITIVE_POINT))
#  ifdef __DPDU__
    return normalize(sd->dPdu);
#  else
    return make_float3(0.0f, 0.0f, 0.0f);
#  endif
#endif

  /* try to create spherical tangent from generated coordinates */
  const AttributeDescriptor desc = find_attribute(kg, sd, ATTR_STD_GENERATED);

  if (desc.offset != ATTR_STD_NOT_FOUND) {
    float3 data = primitive_surface_attribute_float3(kg, sd, desc, NULL, NULL);
    data = make_float3(-(data.y - 0.5f), (data.x - 0.5f), 0.0f);
    object_normal_transform(kg, sd, &data);
    return cross(sd->N, normalize(cross(data, sd->N)));
  }
  else {
    /* otherwise use surface derivatives */
#ifdef __DPDU__
    return normalize(sd->dPdu);
#else
    return make_float3(0.0f, 0.0f, 0.0f);
#endif
  }
}

/* Motion vector for motion pass */

ccl_device_forceinline float4 primitive_motion_vector(KernelGlobals kg,
                                                      ccl_private const ShaderData *sd)
{
  /* center position */
  float3 center;

#if defined(__HAIR__) || defined(__POINTCLOUD__)
  bool is_curve_or_point = sd->type & (PRIMITIVE_CURVE | PRIMITIVE_POINT);
  if (is_curve_or_point) {
    center = make_float3(0.0f, 0.0f, 0.0f);

    if (sd->type & PRIMITIVE_CURVE) {
#  if defined(__HAIR__)
      center = curve_motion_center_location(kg, sd);
#  endif
    }
    else if (sd->type & PRIMITIVE_POINT) {
#  if defined(__POINTCLOUD__)
      center = point_motion_center_location(kg, sd);
#  endif
    }

    if (!(sd->object_flag & SD_OBJECT_TRANSFORM_APPLIED)) {
      object_position_transform(kg, sd, &center);
    }
  }
  else
#endif
  {
    center = sd->P;
  }

  float3 motion_pre = center, motion_post = center;

  /* deformation motion */
  AttributeDescriptor desc = find_attribute(kg, sd, ATTR_STD_MOTION_VERTEX_POSITION);

  if (desc.offset != ATTR_STD_NOT_FOUND) {
    /* get motion info */
    int numverts, numkeys;
    object_motion_info(kg, sd->object, NULL, &numverts, &numkeys);

#if defined(__HAIR__) || defined(__POINTCLOUD__)
    if (is_curve_or_point) {
      motion_pre = float4_to_float3(curve_attribute_float4(kg, sd, desc, NULL, NULL));
      desc.offset += numkeys;
      motion_post = float4_to_float3(curve_attribute_float4(kg, sd, desc, NULL, NULL));

      /* Curve */
      if ((sd->object_flag & SD_OBJECT_HAS_VERTEX_MOTION) == 0) {
        object_position_transform(kg, sd, &motion_pre);
        object_position_transform(kg, sd, &motion_post);
      }
    }
    else
#endif
        if (sd->type & PRIMITIVE_TRIANGLE) {
      /* Triangle */
      if (subd_triangle_patch(kg, sd) == ~0) {
        motion_pre = triangle_attribute_float3(kg, sd, desc, NULL, NULL);
        desc.offset += numverts;
        motion_post = triangle_attribute_float3(kg, sd, desc, NULL, NULL);
      }
      else {
        motion_pre = subd_triangle_attribute_float3(kg, sd, desc, NULL, NULL);
        desc.offset += numverts;
        motion_post = subd_triangle_attribute_float3(kg, sd, desc, NULL, NULL);
      }
    }
  }

  /* object motion. note that depending on the mesh having motion vectors, this
   * transformation was set match the world/object space of motion_pre/post */
  Transform tfm;

  tfm = object_fetch_motion_pass_transform(kg, sd->object, OBJECT_PASS_MOTION_PRE);
  motion_pre = transform_point(&tfm, motion_pre);

  tfm = object_fetch_motion_pass_transform(kg, sd->object, OBJECT_PASS_MOTION_POST);
  motion_post = transform_point(&tfm, motion_post);

  float3 motion_center;

  /* camera motion, for perspective/orthographic motion.pre/post will be a
   * world-to-raster matrix, for panorama it's world-to-camera */
  if (kernel_data.cam.type != CAMERA_PANORAMA) {
    ProjectionTransform projection = kernel_data.cam.worldtoraster;
    motion_center = transform_perspective(&projection, center);

    projection = kernel_data.cam.perspective_pre;
    motion_pre = transform_perspective(&projection, motion_pre);

    projection = kernel_data.cam.perspective_post;
    motion_post = transform_perspective(&projection, motion_post);
  }
  else {
    tfm = kernel_data.cam.worldtocamera;
    motion_center = normalize(transform_point(&tfm, center));
    motion_center = float2_to_float3(direction_to_panorama(&kernel_data.cam, motion_center));
    motion_center.x *= kernel_data.cam.width;
    motion_center.y *= kernel_data.cam.height;

    tfm = kernel_data.cam.motion_pass_pre;
    motion_pre = normalize(transform_point(&tfm, motion_pre));
    motion_pre = float2_to_float3(direction_to_panorama(&kernel_data.cam, motion_pre));
    motion_pre.x *= kernel_data.cam.width;
    motion_pre.y *= kernel_data.cam.height;

    tfm = kernel_data.cam.motion_pass_post;
    motion_post = normalize(transform_point(&tfm, motion_post));
    motion_post = float2_to_float3(direction_to_panorama(&kernel_data.cam, motion_post));
    motion_post.x *= kernel_data.cam.width;
    motion_post.y *= kernel_data.cam.height;
  }

  motion_pre = motion_pre - motion_center;
  motion_post = motion_center - motion_post;

  return make_float4(motion_pre.x, motion_pre.y, motion_post.x, motion_post.y);
}

CCL_NAMESPACE_END
