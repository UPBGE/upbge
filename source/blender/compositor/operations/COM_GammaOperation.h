/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. */

#pragma once

#include "COM_MultiThreadedRowOperation.h"

namespace blender::compositor {

class GammaOperation : public MultiThreadedRowOperation {
 private:
  /**
   * Cached reference to the input_program
   */
  SocketReader *input_program_;
  SocketReader *input_gamma_program_;

 public:
  GammaOperation();

  /**
   * The inner loop of this operation.
   */
  void execute_pixel_sampled(float output[4], float x, float y, PixelSampler sampler) override;

  /**
   * Initialize the execution
   */
  void init_execution() override;

  /**
   * Deinitialize the execution
   */
  void deinit_execution() override;

  void update_memory_buffer_row(PixelCursor &p) override;
};

}  // namespace blender::compositor
