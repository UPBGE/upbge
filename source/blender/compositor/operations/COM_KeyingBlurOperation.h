/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2012 Blender Foundation. */

#pragma once

#include "COM_MultiThreadedOperation.h"

namespace blender::compositor {

/**
 * Class with implementation of blurring for keying node
 */
class KeyingBlurOperation : public MultiThreadedOperation {
 protected:
  int size_;
  int axis_;

 public:
  enum BlurAxis {
    BLUR_AXIS_X = 0,
    BLUR_AXIS_Y = 1,
  };

  KeyingBlurOperation();

  void set_size(int value)
  {
    size_ = value;
  }
  void set_axis(int value)
  {
    axis_ = value;
  }

  void *initialize_tile_data(rcti *rect) override;

  void execute_pixel(float output[4], int x, int y, void *data) override;

  bool determine_depending_area_of_interest(rcti *input,
                                            ReadBufferOperation *read_operation,
                                            rcti *output) override;

  void get_area_of_interest(int input_idx, const rcti &output_area, rcti &r_input_area) override;
  void update_memory_buffer_partial(MemoryBuffer *output,
                                    const rcti &area,
                                    Span<MemoryBuffer *> inputs) override;
};

}  // namespace blender::compositor
