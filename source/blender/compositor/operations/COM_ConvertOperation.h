/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. */

#pragma once

#include "COM_MultiThreadedOperation.h"

namespace blender::compositor {

class ConvertBaseOperation : public MultiThreadedOperation {
 protected:
  SocketReader *input_operation_;

 public:
  ConvertBaseOperation();

  void init_execution() override;
  void deinit_execution() override;

  void update_memory_buffer_partial(MemoryBuffer *output,
                                    const rcti &area,
                                    Span<MemoryBuffer *> inputs) final;

 protected:
  virtual void hash_output_params() override;
  virtual void update_memory_buffer_partial(BuffersIterator<float> &it) = 0;
};

class ConvertValueToColorOperation : public ConvertBaseOperation {
 public:
  ConvertValueToColorOperation();

  void execute_pixel_sampled(float output[4], float x, float y, PixelSampler sampler) override;

 protected:
  void update_memory_buffer_partial(BuffersIterator<float> &it) override;
};

class ConvertColorToValueOperation : public ConvertBaseOperation {
 public:
  ConvertColorToValueOperation();

  void execute_pixel_sampled(float output[4], float x, float y, PixelSampler sampler) override;

 protected:
  void update_memory_buffer_partial(BuffersIterator<float> &it) override;
};

class ConvertColorToBWOperation : public ConvertBaseOperation {
 public:
  ConvertColorToBWOperation();

  void execute_pixel_sampled(float output[4], float x, float y, PixelSampler sampler) override;

 protected:
  void update_memory_buffer_partial(BuffersIterator<float> &it) override;
};

class ConvertColorToVectorOperation : public ConvertBaseOperation {
 public:
  ConvertColorToVectorOperation();

  void execute_pixel_sampled(float output[4], float x, float y, PixelSampler sampler) override;

 protected:
  void update_memory_buffer_partial(BuffersIterator<float> &it) override;
};

class ConvertValueToVectorOperation : public ConvertBaseOperation {
 public:
  ConvertValueToVectorOperation();

  void execute_pixel_sampled(float output[4], float x, float y, PixelSampler sampler) override;

 protected:
  void update_memory_buffer_partial(BuffersIterator<float> &it) override;
};

class ConvertVectorToColorOperation : public ConvertBaseOperation {
 public:
  ConvertVectorToColorOperation();

  void execute_pixel_sampled(float output[4], float x, float y, PixelSampler sampler) override;

 protected:
  void update_memory_buffer_partial(BuffersIterator<float> &it) override;
};

class ConvertVectorToValueOperation : public ConvertBaseOperation {
 public:
  ConvertVectorToValueOperation();

  void execute_pixel_sampled(float output[4], float x, float y, PixelSampler sampler) override;

 protected:
  void update_memory_buffer_partial(BuffersIterator<float> &it) override;
};

class ConvertRGBToYCCOperation : public ConvertBaseOperation {
 private:
  /** YCbCr mode (Jpeg, ITU601, ITU709) */
  int mode_;

 public:
  ConvertRGBToYCCOperation();

  void execute_pixel_sampled(float output[4], float x, float y, PixelSampler sampler) override;

  /** Set the YCC mode */
  void set_mode(int mode);

 protected:
  void hash_output_params() override;
  void update_memory_buffer_partial(BuffersIterator<float> &it) override;
};

class ConvertYCCToRGBOperation : public ConvertBaseOperation {
 private:
  /** YCbCr mode (Jpeg, ITU601, ITU709) */
  int mode_;

 public:
  ConvertYCCToRGBOperation();

  void execute_pixel_sampled(float output[4], float x, float y, PixelSampler sampler) override;

  /** Set the YCC mode */
  void set_mode(int mode);

 protected:
  void hash_output_params() override;
  void update_memory_buffer_partial(BuffersIterator<float> &it) override;
};

class ConvertRGBToYUVOperation : public ConvertBaseOperation {
 public:
  ConvertRGBToYUVOperation();

  void execute_pixel_sampled(float output[4], float x, float y, PixelSampler sampler) override;

 protected:
  void update_memory_buffer_partial(BuffersIterator<float> &it) override;
};

class ConvertYUVToRGBOperation : public ConvertBaseOperation {
 public:
  ConvertYUVToRGBOperation();

  void execute_pixel_sampled(float output[4], float x, float y, PixelSampler sampler) override;

 protected:
  void update_memory_buffer_partial(BuffersIterator<float> &it) override;
};

class ConvertRGBToHSVOperation : public ConvertBaseOperation {
 public:
  ConvertRGBToHSVOperation();

  void execute_pixel_sampled(float output[4], float x, float y, PixelSampler sampler) override;

 protected:
  void update_memory_buffer_partial(BuffersIterator<float> &it) override;
};

class ConvertHSVToRGBOperation : public ConvertBaseOperation {
 public:
  ConvertHSVToRGBOperation();

  void execute_pixel_sampled(float output[4], float x, float y, PixelSampler sampler) override;

 protected:
  void update_memory_buffer_partial(BuffersIterator<float> &it) override;
};

class ConvertRGBToHSLOperation : public ConvertBaseOperation {
 public:
  ConvertRGBToHSLOperation();

  void execute_pixel_sampled(float output[4], float x, float y, PixelSampler sampler) override;

 protected:
  void update_memory_buffer_partial(BuffersIterator<float> &it) override;
};

class ConvertHSLToRGBOperation : public ConvertBaseOperation {
 public:
  ConvertHSLToRGBOperation();

  void execute_pixel_sampled(float output[4], float x, float y, PixelSampler sampler) override;

 protected:
  void update_memory_buffer_partial(BuffersIterator<float> &it) override;
};

class ConvertPremulToStraightOperation : public ConvertBaseOperation {
 public:
  ConvertPremulToStraightOperation();

  void execute_pixel_sampled(float output[4], float x, float y, PixelSampler sampler) override;

 protected:
  void update_memory_buffer_partial(BuffersIterator<float> &it) override;
};

class ConvertStraightToPremulOperation : public ConvertBaseOperation {
 public:
  ConvertStraightToPremulOperation();

  void execute_pixel_sampled(float output[4], float x, float y, PixelSampler sampler) override;

 protected:
  void update_memory_buffer_partial(BuffersIterator<float> &it) override;
};

class SeparateChannelOperation : public MultiThreadedOperation {
 private:
  SocketReader *input_operation_;
  int channel_;

 public:
  SeparateChannelOperation();
  void execute_pixel_sampled(float output[4], float x, float y, PixelSampler sampler) override;

  void init_execution() override;
  void deinit_execution() override;

  void set_channel(int channel)
  {
    channel_ = channel;
  }

  void update_memory_buffer_partial(MemoryBuffer *output,
                                    const rcti &area,
                                    Span<MemoryBuffer *> inputs) override;
};

class CombineChannelsOperation : public MultiThreadedOperation {
 private:
  SocketReader *input_channel1_operation_;
  SocketReader *input_channel2_operation_;
  SocketReader *input_channel3_operation_;
  SocketReader *input_channel4_operation_;

 public:
  CombineChannelsOperation();
  void execute_pixel_sampled(float output[4], float x, float y, PixelSampler sampler) override;

  void init_execution() override;
  void deinit_execution() override;

  void update_memory_buffer_partial(MemoryBuffer *output,
                                    const rcti &area,
                                    Span<MemoryBuffer *> inputs) override;
};

}  // namespace blender::compositor
