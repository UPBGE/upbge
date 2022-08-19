/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. */

#pragma once

#include "COM_MultiThreadedOperation.h"
#include "COM_QualityStepHelper.h"

#define MAX_GAUSSTAB_RADIUS 30000

#include "BLI_simd.h"

namespace blender::compositor {

class BlurBaseOperation : public MultiThreadedOperation, public QualityStepHelper {
 private:
  bool extend_bounds_;

 protected:
  static constexpr int IMAGE_INPUT_INDEX = 0;
  static constexpr int SIZE_INPUT_INDEX = 1;

 protected:
  BlurBaseOperation(DataType data_type8);
  float *make_gausstab(float rad, int size);
#ifdef BLI_HAVE_SSE2
  __m128 *convert_gausstab_sse(const float *gausstab, int size);
#endif
  /**
   * Normalized distance from the current (inverted so 1.0 is close and 0.0 is far)
   * 'ease' is applied after, looks nicer.
   */
  float *make_dist_fac_inverse(float rad, int size, int falloff);

  void update_size();

  /**
   * Cached reference to the input_program
   */
  SocketReader *input_program_;
  SocketReader *input_size_;
  NodeBlurData data_;

  float size_;
  bool sizeavailable_;

  /* Flags for inheriting classes. */
  bool use_variable_size_;

 public:
  virtual void init_data() override;
  /**
   * Initialize the execution
   */
  void init_execution() override;

  /**
   * Deinitialize the execution
   */
  void deinit_execution() override;

  void set_data(const NodeBlurData *data);

  void set_size(float size)
  {
    size_ = size;
    sizeavailable_ = true;
  }

  void set_extend_bounds(bool extend_bounds)
  {
    extend_bounds_ = extend_bounds;
  }

  int get_blur_size(eDimension dim) const;

  void determine_canvas(const rcti &preferred_area, rcti &r_area) override;

  virtual void get_area_of_interest(int input_idx,
                                    const rcti &output_area,
                                    rcti &r_input_area) override;
};

}  // namespace blender::compositor
