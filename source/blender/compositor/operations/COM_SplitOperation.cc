/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. */

#include "COM_SplitOperation.h"

namespace blender::compositor {

SplitOperation::SplitOperation()
{
  this->add_input_socket(DataType::Color);
  this->add_input_socket(DataType::Color);
  this->add_output_socket(DataType::Color);
  image1Input_ = nullptr;
  image2Input_ = nullptr;
}

void SplitOperation::init_execution()
{
  /* When initializing the tree during initial load the width and height can be zero. */
  image1Input_ = get_input_socket_reader(0);
  image2Input_ = get_input_socket_reader(1);
}

void SplitOperation::deinit_execution()
{
  image1Input_ = nullptr;
  image2Input_ = nullptr;
}

void SplitOperation::execute_pixel_sampled(float output[4],
                                           float x,
                                           float y,
                                           PixelSampler /*sampler*/)
{
  int perc = x_split_ ? split_percentage_ * this->get_width() / 100.0f :
                        split_percentage_ * this->get_height() / 100.0f;
  bool image1 = x_split_ ? x > perc : y > perc;
  if (image1) {
    image1Input_->read_sampled(output, x, y, PixelSampler::Nearest);
  }
  else {
    image2Input_->read_sampled(output, x, y, PixelSampler::Nearest);
  }
}

void SplitOperation::determine_canvas(const rcti &preferred_area, rcti &r_area)
{
  rcti unused_area = COM_AREA_NONE;

  const bool determined = this->get_input_socket(0)->determine_canvas(COM_AREA_NONE, unused_area);
  this->set_canvas_input_index(determined ? 0 : 1);

  NodeOperation::determine_canvas(preferred_area, r_area);
}

void SplitOperation::update_memory_buffer_partial(MemoryBuffer *output,
                                                  const rcti &area,
                                                  Span<MemoryBuffer *> inputs)
{
  const int percent = x_split_ ? split_percentage_ * this->get_width() / 100.0f :
                                 split_percentage_ * this->get_height() / 100.0f;
  const size_t elem_bytes = COM_data_type_bytes_len(get_output_socket()->get_data_type());
  for (BuffersIterator<float> it = output->iterate_with(inputs, area); !it.is_end(); ++it) {
    const bool is_image1 = x_split_ ? it.x > percent : it.y > percent;
    memcpy(it.out, it.in(is_image1 ? 0 : 1), elem_bytes);
  }
}

}  // namespace blender::compositor
