/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Primitive Utilities
 *
 * Generic functions to look up mesh, curve and volume primitive attributes for
 * shading and render passes. */

#pragma once

#include "kernel/globals.h"

#include "kernel/camera/projection.h"

#include "kernel/geom/attribute.h"
#include "kernel/geom/curve.h"
#include "kernel/geom/object.h"
#include "kernel/geom/point.h"
#include "kernel/geom/triangle.h"
#include "kernel/geom/volume.h"

CCL_NAMESPACE_BEGIN

/* Surface Attributes
 *
 * Read geometry attributes for surface shading. This is distinct from volume
 * attributes for performance, mainly for GPU performance to avoid bringing in
 * heavy volume interpolation code. */

template<typename T>
ccl_device_forceinline dual<T> primitive_surface_attribute(KernelGlobals kg,
                                                           const ccl_private ShaderData *sd,
                                                           const AttributeDescriptor desc,
                                                           const bool dx = false,
                                                           const bool dy = false)
{
  if (desc.element & (ATTR_ELEMENT_OBJECT | ATTR_ELEMENT_MESH)) {
    return dual<T>(attribute_data_fetch<T>(kg, desc.offset));
  }

  if (sd->type & PRIMITIVE_TRIANGLE) {
    return triangle_attribute<T>(kg, sd, desc, dx, dy);
  }
#ifdef __HAIR__
  if (sd->type & PRIMITIVE_CURVE) {
    return curve_attribute<T>(kg, sd, desc, dx, dy);
  }
#endif
#ifdef __POINTCLOUD__
  else if (sd->type & PRIMITIVE_POINT) {
    return point_attribute<T>(kg, sd, desc, dx, dy);
  }
#endif
  else {
    return make_zero<dual<T>>();
  }
}

#ifdef __VOLUME__
/* Volume Attributes
 *
 * Read geometry attributes for volume shading. This is distinct from surface
 * attributes for performance, mainly for GPU performance to avoid bringing in
 * heavy volume interpolation code. */

ccl_device_forceinline bool primitive_is_volume_attribute(const ccl_private ShaderData *sd)
{
  return sd->type == PRIMITIVE_VOLUME;
}

template<typename T>
ccl_device_inline T primitive_volume_attribute(KernelGlobals kg,
                                               ccl_private ShaderData *sd,
                                               const AttributeDescriptor desc,
                                               const bool stochastic)
{
  if (primitive_is_volume_attribute(sd)) {
    return volume_attribute_value<T>(volume_attribute_float4(kg, sd, desc, stochastic));
  }
  return make_zero<T>();
}
#endif

/* Default UV coordinate */

ccl_device_forceinline float3 primitive_uv(KernelGlobals kg, const ccl_private ShaderData *sd)
{
  const AttributeDescriptor desc = find_attribute(kg, sd, ATTR_STD_UV);

  if (desc.offset == ATTR_STD_NOT_FOUND) {
    return make_float3(0.0f, 0.0f, 0.0f);
  }

  const float2 uv = primitive_surface_attribute<float2>(kg, sd, desc).val;
  return make_float3(uv.x, uv.y, 1.0f);
}

/* PTEX coordinates. */

ccl_device bool primitive_ptex(KernelGlobals kg,
                               ccl_private ShaderData *sd,
                               ccl_private float2 *uv,
                               ccl_private int *face_id)
{
  /* storing ptex data as attributes is not memory efficient but simple for tests */
  const AttributeDescriptor desc_face_id = find_attribute(kg, sd, ATTR_STD_PTEX_FACE_ID);
  const AttributeDescriptor desc_uv = find_attribute(kg, sd, ATTR_STD_PTEX_UV);

  if (desc_face_id.offset == ATTR_STD_NOT_FOUND || desc_uv.offset == ATTR_STD_NOT_FOUND) {
    return false;
  }

  const float3 uv3 = primitive_surface_attribute<float3>(kg, sd, desc_uv).val;
  const float face_id_f = primitive_surface_attribute<float>(kg, sd, desc_face_id).val;

  *uv = make_float2(uv3.x, uv3.y);
  *face_id = (int)face_id_f;

  return true;
}

/* Surface tangent */

ccl_device float3 primitive_tangent(KernelGlobals kg, ccl_private ShaderData *sd)
{
#if defined(__HAIR__) || defined(__POINTCLOUD__)
  if (sd->type & (PRIMITIVE_CURVE | PRIMITIVE_POINT)) {
#  ifdef __DPDU__
    return normalize(sd->dPdu);
  }
#  else
    return make_float3(0.0f, 0.0f, 0.0f);
#  endif
#endif

  /* try to create spherical tangent from generated coordinates */
  const AttributeDescriptor desc = find_attribute(kg, sd, ATTR_STD_GENERATED);

  if (desc.offset != ATTR_STD_NOT_FOUND) {
    float3 data = primitive_surface_attribute<float3>(kg, sd, desc).val;
    data = make_float3(-(data.y - 0.5f), (data.x - 0.5f), 0.0f);
    object_normal_transform(kg, sd, &data);
    return cross(sd->N, normalize(cross(data, sd->N)));
  }
  /* otherwise use surface derivatives */
#ifdef __DPDU__
  return normalize(sd->dPdu);
#else
  return make_float3(0.0f, 0.0f, 0.0f);
#endif
}

/* Motion vector for motion pass */

ccl_device_forceinline float4 primitive_motion_vector(KernelGlobals kg,
                                                      const ccl_private ShaderData *sd)
{
  /* center position */
  float3 center;

#if defined(__HAIR__) || defined(__POINTCLOUD__)
  const bool is_curve_or_point = sd->type & (PRIMITIVE_CURVE | PRIMITIVE_POINT);
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

  float3 motion_pre = center;
  float3 motion_post = center;

  /* deformation motion */
  AttributeDescriptor desc = find_attribute(kg, sd, ATTR_STD_MOTION_VERTEX_POSITION);

  if (desc.offset != ATTR_STD_NOT_FOUND) {
    /* get motion info */
    const int numverts = kernel_data_fetch(objects, sd->object).numverts;

#if defined(__HAIR__) || defined(__POINTCLOUD__)
    if (is_curve_or_point) {
      motion_pre = make_float3(primitive_surface_attribute<float4>(kg, sd, desc).val);
      desc.offset += numverts;
      motion_post = make_float3(primitive_surface_attribute<float4>(kg, sd, desc).val);

      /* Curve */
      if ((sd->object_flag & SD_OBJECT_HAS_VERTEX_MOTION) == 0) {
        object_position_transform(kg, sd, &motion_pre);
        object_position_transform(kg, sd, &motion_post);
      }
    }
    else
#endif
        if (sd->type & PRIMITIVE_TRIANGLE)
    {
      /* Triangle */
      motion_pre = triangle_attribute<float3>(kg, sd, desc).val;
      desc.offset += numverts;
      motion_post = triangle_attribute<float3>(kg, sd, desc).val;
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
   * world-to-raster matrix, for panorama it's world-to-camera, for custom
   * we fall back to the world position until we have inverse mapping for it */
  if (kernel_data.cam.type == CAMERA_CUSTOM) {
    /* TODO: Custom cameras don't have inverse mappings yet, so we fall back to
     * camera-space vectors here for now. */
    tfm = kernel_data.cam.worldtocamera;
    motion_center = normalize(transform_point(&tfm, center));

    tfm = kernel_data.cam.motion_pass_pre;
    motion_pre = normalize(transform_point(&tfm, motion_pre));

    tfm = kernel_data.cam.motion_pass_post;
    motion_post = normalize(transform_point(&tfm, motion_post));
  }
  else if (kernel_data.cam.type != CAMERA_PANORAMA) {
    /* Perspective and orthographics camera use the world-to-raster matrix. */
    ProjectionTransform projection = kernel_data.cam.worldtoraster;
    motion_center = transform_perspective(&projection, center);

    projection = kernel_data.cam.perspective_pre;
    motion_pre = transform_perspective(&projection, motion_pre);

    projection = kernel_data.cam.perspective_post;
    motion_post = transform_perspective(&projection, motion_post);
  }
  else {
    /* Panorama cameras have their own inverse mappings. */
    tfm = kernel_data.cam.worldtocamera;
    motion_center = normalize(transform_point(&tfm, center));
    motion_center = make_float3(direction_to_panorama(&kernel_data.cam, motion_center));
    motion_center.x *= kernel_data.cam.width;
    motion_center.y *= kernel_data.cam.height;

    tfm = kernel_data.cam.motion_pass_pre;
    motion_pre = normalize(transform_point(&tfm, motion_pre));
    motion_pre = make_float3(direction_to_panorama(&kernel_data.cam, motion_pre));
    motion_pre.x *= kernel_data.cam.width;
    motion_pre.y *= kernel_data.cam.height;

    tfm = kernel_data.cam.motion_pass_post;
    motion_post = normalize(transform_point(&tfm, motion_post));
    motion_post = make_float3(direction_to_panorama(&kernel_data.cam, motion_post));
    motion_post.x *= kernel_data.cam.width;
    motion_post.y *= kernel_data.cam.height;
  }

  motion_pre = motion_pre - motion_center;
  motion_post = motion_center - motion_post;

  return make_float4(motion_pre.x, motion_pre.y, motion_post.x, motion_post.y);
}

CCL_NAMESPACE_END
