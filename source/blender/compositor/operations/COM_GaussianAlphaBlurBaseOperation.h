/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 Blender Foundation. */

#pragma once

#include "COM_BlurBaseOperation.h"

namespace blender::compositor {

class GaussianAlphaBlurBaseOperation : public BlurBaseOperation {
 protected:
  float *gausstab_;
  float *distbuf_inv_;
  int falloff_; /* Falloff for #distbuf_inv. */
  bool do_subtract_;
  int filtersize_;
  float rad_;
  eDimension dimension_;

 public:
  GaussianAlphaBlurBaseOperation(eDimension dim);

  virtual void init_data() override;
  virtual void init_execution() override;
  virtual void deinit_execution() override;

  void get_area_of_interest(int input_idx, const rcti &output_area, rcti &r_input_area) final;
  void update_memory_buffer_partial(MemoryBuffer *output,
                                    const rcti &area,
                                    Span<MemoryBuffer *> inputs) final;

  /**
   * Set subtract for Dilate/Erode functionality
   */
  void set_subtract(bool subtract)
  {
    do_subtract_ = subtract;
  }
  void set_falloff(int falloff)
  {
    falloff_ = falloff;
  }

  BLI_INLINE float finv_test(const float f, const bool test)
  {
    return (LIKELY(test == false)) ? f : 1.0f - f;
  }
};

}  // namespace blender::compositor
