/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. */

#pragma once

#include "COM_GaussianAlphaBlurBaseOperation.h"

namespace blender::compositor {

/* TODO(manzanilla): everything to be removed with tiled implementation except the constructor. */
class GaussianAlphaYBlurOperation : public GaussianAlphaBlurBaseOperation {
 private:
  void update_gauss();

 public:
  GaussianAlphaYBlurOperation();

  /**
   * The inner loop of this operation.
   */
  void execute_pixel(float output[4], int x, int y, void *data) override;

  /**
   * \brief initialize the execution
   */
  void init_execution() override;

  /**
   * Deinitialize the execution
   */
  void deinit_execution() override;

  void *initialize_tile_data(rcti *rect) override;
  bool determine_depending_area_of_interest(rcti *input,
                                            ReadBufferOperation *read_operation,
                                            rcti *output) override;
};

}  // namespace blender::compositor
