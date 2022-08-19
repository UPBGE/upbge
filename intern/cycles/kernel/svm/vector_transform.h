/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#pragma once

CCL_NAMESPACE_BEGIN

/* Vector Transform */

ccl_device_noinline void svm_node_vector_transform(KernelGlobals kg,
                                                   ccl_private ShaderData *sd,
                                                   ccl_private float *stack,
                                                   uint4 node)
{
  uint itype, ifrom, ito;
  uint vector_in, vector_out;

  svm_unpack_node_uchar3(node.y, &itype, &ifrom, &ito);
  svm_unpack_node_uchar2(node.z, &vector_in, &vector_out);

  float3 in = stack_load_float3(stack, vector_in);

  NodeVectorTransformType type = (NodeVectorTransformType)itype;
  NodeVectorTransformConvertSpace from = (NodeVectorTransformConvertSpace)ifrom;
  NodeVectorTransformConvertSpace to = (NodeVectorTransformConvertSpace)ito;

  Transform tfm;
  bool is_object = (sd->object != OBJECT_NONE);
  bool is_direction = (type == NODE_VECTOR_TRANSFORM_TYPE_VECTOR ||
                       type == NODE_VECTOR_TRANSFORM_TYPE_NORMAL);

  /* From world */
  if (from == NODE_VECTOR_TRANSFORM_CONVERT_SPACE_WORLD) {
    if (to == NODE_VECTOR_TRANSFORM_CONVERT_SPACE_CAMERA) {
      tfm = kernel_data.cam.worldtocamera;
      if (is_direction)
        in = transform_direction(&tfm, in);
      else
        in = transform_point(&tfm, in);
    }
    else if (to == NODE_VECTOR_TRANSFORM_CONVERT_SPACE_OBJECT && is_object) {
      if (is_direction)
        object_inverse_dir_transform(kg, sd, &in);
      else
        object_inverse_position_transform(kg, sd, &in);
    }
  }

  /* From camera */
  else if (from == NODE_VECTOR_TRANSFORM_CONVERT_SPACE_CAMERA) {
    if (to == NODE_VECTOR_TRANSFORM_CONVERT_SPACE_WORLD ||
        to == NODE_VECTOR_TRANSFORM_CONVERT_SPACE_OBJECT) {
      tfm = kernel_data.cam.cameratoworld;
      if (is_direction)
        in = transform_direction(&tfm, in);
      else
        in = transform_point(&tfm, in);
    }
    if (to == NODE_VECTOR_TRANSFORM_CONVERT_SPACE_OBJECT && is_object) {
      if (is_direction)
        object_inverse_dir_transform(kg, sd, &in);
      else
        object_inverse_position_transform(kg, sd, &in);
    }
  }

  /* From object */
  else if (from == NODE_VECTOR_TRANSFORM_CONVERT_SPACE_OBJECT) {
    if ((to == NODE_VECTOR_TRANSFORM_CONVERT_SPACE_WORLD ||
         to == NODE_VECTOR_TRANSFORM_CONVERT_SPACE_CAMERA) &&
        is_object) {
      if (is_direction)
        object_dir_transform(kg, sd, &in);
      else
        object_position_transform(kg, sd, &in);
    }
    if (to == NODE_VECTOR_TRANSFORM_CONVERT_SPACE_CAMERA) {
      tfm = kernel_data.cam.worldtocamera;
      if (is_direction)
        in = transform_direction(&tfm, in);
      else
        in = transform_point(&tfm, in);
    }
  }

  /* Normalize Normal */
  if (type == NODE_VECTOR_TRANSFORM_TYPE_NORMAL)
    in = normalize(in);

  /* Output */
  if (stack_valid(vector_out)) {
    stack_store_float3(stack, vector_out, in);
  }
}

CCL_NAMESPACE_END
