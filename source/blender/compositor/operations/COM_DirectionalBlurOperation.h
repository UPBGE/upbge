/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. */

#pragma once

#include "COM_MultiThreadedOperation.h"
#include "COM_QualityStepHelper.h"

namespace blender::compositor {

class DirectionalBlurOperation : public MultiThreadedOperation, public QualityStepHelper {
 private:
  SocketReader *input_program_;
  NodeDBlurData *data_;

  float center_x_pix_, center_y_pix_;
  float tx_, ty_;
  float sc_, rot_;

 public:
  DirectionalBlurOperation();

  /**
   * The inner loop of this operation.
   */
  void execute_pixel(float output[4], int x, int y, void *data) override;

  /**
   * Initialize the execution
   */
  void init_execution() override;

  /**
   * Deinitialize the execution
   */
  void deinit_execution() override;

  bool determine_depending_area_of_interest(rcti *input,
                                            ReadBufferOperation *read_operation,
                                            rcti *output) override;

  void set_data(NodeDBlurData *data)
  {
    data_ = data;
  }

  void execute_opencl(OpenCLDevice *device,
                      MemoryBuffer *output_memory_buffer,
                      cl_mem cl_output_buffer,
                      MemoryBuffer **input_memory_buffers,
                      std::list<cl_mem> *cl_mem_to_clean_up,
                      std::list<cl_kernel> *cl_kernels_to_clean_up) override;

  void get_area_of_interest(int input_idx, const rcti &output_area, rcti &r_input_area) override;
  void update_memory_buffer_partial(MemoryBuffer *output,
                                    const rcti &area,
                                    Span<MemoryBuffer *> inputs) override;
};

}  // namespace blender::compositor
