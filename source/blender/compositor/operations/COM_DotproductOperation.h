/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. */

#pragma once

#include "COM_MultiThreadedOperation.h"

namespace blender::compositor {

class DotproductOperation : public MultiThreadedOperation {
 private:
  SocketReader *input1Operation_;
  SocketReader *input2Operation_;

 public:
  DotproductOperation();
  void execute_pixel_sampled(float output[4], float x, float y, PixelSampler sampler) override;

  void init_execution() override;
  void deinit_execution() override;

  void update_memory_buffer_partial(MemoryBuffer *output,
                                    const rcti &area,
                                    Span<MemoryBuffer *> inputs) override;
};

}  // namespace blender::compositor
