/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. */

#pragma once

#include "COM_CurveBaseOperation.h"
#include "COM_NodeOperation.h"

namespace blender::compositor {

class VectorCurveOperation : public CurveBaseOperation {
 private:
  /**
   * Cached reference to the input_program
   */
  SocketReader *input_program_;

 public:
  VectorCurveOperation();

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

  void update_memory_buffer_partial(MemoryBuffer *output,
                                    const rcti &area,
                                    Span<MemoryBuffer *> inputs) override;
};

}  // namespace blender::compositor
