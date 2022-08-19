/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. */

#include "COM_AlphaOverMixedOperation.h"

namespace blender::compositor {

AlphaOverMixedOperation::AlphaOverMixedOperation()
{
  x_ = 0.0f;
  flags_.can_be_constant = true;
}

void AlphaOverMixedOperation::execute_pixel_sampled(float output[4],
                                                    float x,
                                                    float y,
                                                    PixelSampler sampler)
{
  float input_color1[4];
  float input_over_color[4];
  float value[4];

  input_value_operation_->read_sampled(value, x, y, sampler);
  input_color1_operation_->read_sampled(input_color1, x, y, sampler);
  input_color2_operation_->read_sampled(input_over_color, x, y, sampler);

  if (input_over_color[3] <= 0.0f) {
    copy_v4_v4(output, input_color1);
  }
  else if (value[0] == 1.0f && input_over_color[3] >= 1.0f) {
    copy_v4_v4(output, input_over_color);
  }
  else {
    float addfac = 1.0f - x_ + input_over_color[3] * x_;
    float premul = value[0] * addfac;
    float mul = 1.0f - value[0] * input_over_color[3];

    output[0] = (mul * input_color1[0]) + premul * input_over_color[0];
    output[1] = (mul * input_color1[1]) + premul * input_over_color[1];
    output[2] = (mul * input_color1[2]) + premul * input_over_color[2];
    output[3] = (mul * input_color1[3]) + value[0] * input_over_color[3];
  }
}

void AlphaOverMixedOperation::update_memory_buffer_row(PixelCursor &p)
{
  for (; p.out < p.row_end; p.next()) {
    const float *color1 = p.color1;
    const float *over_color = p.color2;
    const float value = *p.value;

    if (over_color[3] <= 0.0f) {
      copy_v4_v4(p.out, color1);
    }
    else if (value == 1.0f && over_color[3] >= 1.0f) {
      copy_v4_v4(p.out, over_color);
    }
    else {
      const float addfac = 1.0f - x_ + over_color[3] * x_;
      const float premul = value * addfac;
      const float mul = 1.0f - value * over_color[3];

      p.out[0] = (mul * color1[0]) + premul * over_color[0];
      p.out[1] = (mul * color1[1]) + premul * over_color[1];
      p.out[2] = (mul * color1[2]) + premul * over_color[2];
      p.out[3] = (mul * color1[3]) + value * over_color[3];
    }
  }
}

}  // namespace blender::compositor
