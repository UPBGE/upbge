/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#pragma once

CCL_NAMESPACE_BEGIN

ccl_device_noinline void svm_node_hsv(KernelGlobals kg,
                                      ccl_private ShaderData *sd,
                                      ccl_private float *stack,
                                      uint4 node)
{
  uint in_color_offset, fac_offset, out_color_offset;
  uint hue_offset, sat_offset, val_offset;
  svm_unpack_node_uchar3(node.y, &in_color_offset, &fac_offset, &out_color_offset);
  svm_unpack_node_uchar3(node.z, &hue_offset, &sat_offset, &val_offset);

  float fac = stack_load_float(stack, fac_offset);
  float3 in_color = stack_load_float3(stack, in_color_offset);
  float3 color = in_color;

  float hue = stack_load_float(stack, hue_offset);
  float sat = stack_load_float(stack, sat_offset);
  float val = stack_load_float(stack, val_offset);

  color = rgb_to_hsv(color);

  /* Remember: `fmodf` doesn't work for negative numbers here. */
  color.x = fmodf(color.x + hue + 0.5f, 1.0f);
  color.y = saturatef(color.y * sat);
  color.z *= val;

  color = hsv_to_rgb(color);

  color.x = fac * color.x + (1.0f - fac) * in_color.x;
  color.y = fac * color.y + (1.0f - fac) * in_color.y;
  color.z = fac * color.z + (1.0f - fac) * in_color.z;

  /* Clamp color to prevent negative values caused by over saturation. */
  color.x = max(color.x, 0.0f);
  color.y = max(color.y, 0.0f);
  color.z = max(color.z, 0.0f);

  if (stack_valid(out_color_offset))
    stack_store_float3(stack, out_color_offset, color);
}

CCL_NAMESPACE_END
