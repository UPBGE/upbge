/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#pragma once

CCL_NAMESPACE_BEGIN

/* Clamp Node */

ccl_device_noinline int svm_node_clamp(KernelGlobals kg,
                                       ccl_private ShaderData *sd,
                                       ccl_private float *stack,
                                       uint value_stack_offset,
                                       uint parameters_stack_offsets,
                                       uint result_stack_offset,
                                       int offset)
{
  uint min_stack_offset, max_stack_offset, type;
  svm_unpack_node_uchar3(parameters_stack_offsets, &min_stack_offset, &max_stack_offset, &type);

  uint4 defaults = read_node(kg, &offset);

  float value = stack_load_float(stack, value_stack_offset);
  float min = stack_load_float_default(stack, min_stack_offset, defaults.x);
  float max = stack_load_float_default(stack, max_stack_offset, defaults.y);

  if (type == NODE_CLAMP_RANGE && (min > max)) {
    stack_store_float(stack, result_stack_offset, clamp(value, max, min));
  }
  else {
    stack_store_float(stack, result_stack_offset, clamp(value, min, max));
  }
  return offset;
}

CCL_NAMESPACE_END
