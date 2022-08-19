/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#pragma once

CCL_NAMESPACE_BEGIN

/* Value Nodes */

ccl_device void svm_node_value_f(KernelGlobals kg,
                                 ccl_private ShaderData *sd,
                                 ccl_private float *stack,
                                 uint ivalue,
                                 uint out_offset)
{
  stack_store_float(stack, out_offset, __uint_as_float(ivalue));
}

ccl_device int svm_node_value_v(KernelGlobals kg,
                                ccl_private ShaderData *sd,
                                ccl_private float *stack,
                                uint out_offset,
                                int offset)
{
  /* read extra data */
  uint4 node1 = read_node(kg, &offset);
  float3 p = make_float3(
      __uint_as_float(node1.y), __uint_as_float(node1.z), __uint_as_float(node1.w));

  stack_store_float3(stack, out_offset, p);
  return offset;
}

CCL_NAMESPACE_END
