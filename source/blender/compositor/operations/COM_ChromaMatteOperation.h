/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. */

#pragma once

#include "COM_MultiThreadedOperation.h"

namespace blender::compositor {

/**
 * this program converts an input color to an output value.
 * it assumes we are in sRGB color space.
 */
class ChromaMatteOperation : public MultiThreadedOperation {
 private:
  NodeChroma *settings_;
  SocketReader *input_image_program_;
  SocketReader *input_key_program_;

 public:
  /**
   * Default constructor
   */
  ChromaMatteOperation();

  /**
   * The inner loop of this operation.
   */
  void execute_pixel_sampled(float output[4], float x, float y, PixelSampler sampler) override;

  void init_execution() override;
  void deinit_execution() override;

  void set_settings(NodeChroma *node_chroma)
  {
    settings_ = node_chroma;
  }

  void update_memory_buffer_partial(MemoryBuffer *output,
                                    const rcti &area,
                                    Span<MemoryBuffer *> inputs) override;
};

}  // namespace blender::compositor
