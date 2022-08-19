/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. */

#pragma once

#include "COM_BufferArea.h"
#include "COM_BufferRange.h"
#include "COM_BuffersIterator.h"
#include "COM_Enums.h"

#include "BLI_math_interp.h"
#include "BLI_rect.h"

#include "IMB_colormanagement.h"

struct ImBuf;

namespace blender::compositor {

/**
 * \brief state of a memory buffer
 * \ingroup Memory
 */
enum class MemoryBufferState {
  /** \brief memory has been allocated on creator device and CPU machine,
   * but kernel has not been executed */
  Default = 0,
  /** \brief chunk is consolidated from other chunks. special state. */
  Temporary = 6,
};

enum class MemoryBufferExtend {
  Clip,
  Extend,
  Repeat,
};

class MemoryProxy;

/**
 * \brief a MemoryBuffer contains access to the data of a chunk
 */
class MemoryBuffer {
 public:
  /**
   * Offset between elements.
   *
   * Should always be used for the x dimension when calculating buffer offsets.
   * It will be 0 when is_a_single_elem=true.
   * e.g: buffer_index = y * buffer.row_stride + x * buffer.elem_stride
   */
  int elem_stride;

  /**
   * Offset between rows.
   *
   * Should always be used for the y dimension when calculating buffer offsets.
   * It will be 0 when is_a_single_elem=true.
   * e.g: buffer_index = y * buffer.row_stride + x * buffer.elem_stride
   */
  int row_stride;

 private:
  /**
   * \brief proxy of the memory (same for all chunks in the same buffer)
   */
  MemoryProxy *memory_proxy_;

  /**
   * \brief the type of buffer DataType::Value, DataType::Vector, DataType::Color
   */
  DataType datatype_;

  /**
   * \brief region of this buffer inside relative to the MemoryProxy
   */
  rcti rect_;

  /**
   * \brief state of the buffer
   */
  MemoryBufferState state_;

  /**
   * \brief the actual float buffer/data
   */
  float *buffer_;

  /**
   * \brief the number of channels of a single value in the buffer.
   * For value buffers this is 1, vector 3 and color 4
   */
  uint8_t num_channels_;

  /**
   * Whether buffer is a single element in memory.
   */
  bool is_a_single_elem_;

  /**
   * Whether MemoryBuffer owns buffer data.
   */
  bool owns_data_;

  /** Stride to make any x coordinate within buffer positive (non-zero). */
  int to_positive_x_stride_;

  /** Stride to make any y coordinate within buffer positive (non-zero). */
  int to_positive_y_stride_;

 public:
  /**
   * \brief construct new temporarily MemoryBuffer for an area
   */
  MemoryBuffer(MemoryProxy *memory_proxy, const rcti &rect, MemoryBufferState state);

  /**
   * \brief construct new temporarily MemoryBuffer for an area
   */
  MemoryBuffer(DataType data_type, const rcti &rect, bool is_a_single_elem = false);

  /**
   * Construct MemoryBuffer from a float buffer. MemoryBuffer is not responsible for
   * freeing it.
   */
  MemoryBuffer(
      float *buffer, int num_channels, int width, int height, bool is_a_single_elem = false);

  /**
   * Construct MemoryBuffer from a float buffer area. MemoryBuffer is not responsible for
   * freeing given buffer.
   */
  MemoryBuffer(float *buffer, int num_channels, const rcti &rect, bool is_a_single_elem = false);

  /**
   * Copy constructor
   */
  MemoryBuffer(const MemoryBuffer &src);

  /**
   * \brief destructor
   */
  ~MemoryBuffer();

  /**
   * Whether buffer is a single element in memory independently of its resolution. True for set
   * operations buffers.
   */
  bool is_a_single_elem() const
  {
    return is_a_single_elem_;
  }

  float &operator[](int index)
  {
    BLI_assert(is_a_single_elem_ ? index < num_channels_ :
                                   index < get_coords_offset(get_width(), get_height()));
    return buffer_[index];
  }

  const float &operator[](int index) const
  {
    BLI_assert(is_a_single_elem_ ? index < num_channels_ :
                                   index < get_coords_offset(get_width(), get_height()));
    return buffer_[index];
  }

  /**
   * Get offset needed to jump from buffer start to given coordinates.
   */
  intptr_t get_coords_offset(int x, int y) const
  {
    return ((intptr_t)y - rect_.ymin) * row_stride + ((intptr_t)x - rect_.xmin) * elem_stride;
  }

  /**
   * Get buffer element at given coordinates.
   */
  float *get_elem(int x, int y)
  {
    BLI_assert(has_coords(x, y));
    return buffer_ + get_coords_offset(x, y);
  }

  /**
   * Get buffer element at given coordinates.
   */
  const float *get_elem(int x, int y) const
  {
    BLI_assert(has_coords(x, y));
    return buffer_ + get_coords_offset(x, y);
  }

  void read_elem(int x, int y, float *out) const
  {
    memcpy(out, get_elem(x, y), get_elem_bytes_len());
  }

  void read_elem_checked(int x, int y, float *out) const
  {
    if (!has_coords(x, y)) {
      clear_elem(out);
    }
    else {
      read_elem(x, y, out);
    }
  }

  void read_elem_checked(float x, float y, float *out) const
  {
    read_elem_checked(floor_x(x), floor_y(y), out);
  }

  void read_elem_bilinear(float x, float y, float *out) const
  {
    /* Only clear past +/-1 borders to be able to smooth edges. */
    if (x <= rect_.xmin - 1.0f || x >= rect_.xmax || y <= rect_.ymin - 1.0f || y >= rect_.ymax) {
      clear_elem(out);
      return;
    }

    if (is_a_single_elem_) {
      if (x >= rect_.xmin && x < rect_.xmax - 1.0f && y >= rect_.ymin && y < rect_.ymax - 1.0f) {
        memcpy(out, buffer_, get_elem_bytes_len());
        return;
      }

      /* Do sampling at borders to smooth edges. */
      const float last_x = get_width() - 1.0f;
      const float rel_x = get_relative_x(x);
      float single_x = 0.0f;
      if (rel_x < 0.0f) {
        single_x = rel_x;
      }
      else if (rel_x > last_x) {
        single_x = rel_x - last_x;
      }

      const float last_y = get_height() - 1.0f;
      const float rel_y = get_relative_y(y);
      float single_y = 0.0f;
      if (rel_y < 0.0f) {
        single_y = rel_y;
      }
      else if (rel_y > last_y) {
        single_y = rel_y - last_y;
      }

      BLI_bilinear_interpolation_fl(buffer_, out, 1, 1, num_channels_, single_x, single_y);
      return;
    }

    BLI_bilinear_interpolation_fl(buffer_,
                                  out,
                                  get_width(),
                                  get_height(),
                                  num_channels_,
                                  get_relative_x(x),
                                  get_relative_y(y));
  }

  void read_elem_sampled(float x, float y, PixelSampler sampler, float *out) const
  {
    switch (sampler) {
      case PixelSampler::Nearest:
        read_elem_checked(x, y, out);
        break;
      case PixelSampler::Bilinear:
      case PixelSampler::Bicubic:
        /* No bicubic. Current implementation produces fuzzy results. */
        read_elem_bilinear(x, y, out);
        break;
    }
  }

  void read_elem_filtered(float x, float y, float dx[2], float dy[2], float *out) const;

  /**
   * Get channel value at given coordinates.
   */
  float &get_value(int x, int y, int channel)
  {
    BLI_assert(has_coords(x, y) && channel >= 0 && channel < num_channels_);
    return buffer_[get_coords_offset(x, y) + channel];
  }

  /**
   * Get channel value at given coordinates.
   */
  const float &get_value(int x, int y, int channel) const
  {
    BLI_assert(has_coords(x, y) && channel >= 0 && channel < num_channels_);
    return buffer_[get_coords_offset(x, y) + channel];
  }

  /**
   * Get the buffer row end.
   */
  const float *get_row_end(int y) const
  {
    BLI_assert(has_y(y));
    return buffer_ + (is_a_single_elem() ? num_channels_ : get_coords_offset(get_width(), y));
  }

  /**
   * Get the number of elements in memory for a row. For single element buffers it will always
   * be 1.
   */
  int get_memory_width() const
  {
    return is_a_single_elem() ? 1 : get_width();
  }

  /**
   * Get number of elements in memory for a column. For single element buffers it will
   * always be 1.
   */
  int get_memory_height() const
  {
    return is_a_single_elem() ? 1 : get_height();
  }

  uint8_t get_num_channels() const
  {
    return num_channels_;
  }

  uint8_t get_elem_bytes_len() const
  {
    return num_channels_ * sizeof(float);
  }

  /**
   * Get all buffer elements as a range with no offsets.
   */
  BufferRange<float> as_range()
  {
    return BufferRange<float>(buffer_, 0, buffer_len(), elem_stride);
  }

  BufferRange<const float> as_range() const
  {
    return BufferRange<const float>(buffer_, 0, buffer_len(), elem_stride);
  }

  BufferArea<float> get_buffer_area(const rcti &area)
  {
    return BufferArea<float>(buffer_, get_width(), area, elem_stride);
  }

  BufferArea<const float> get_buffer_area(const rcti &area) const
  {
    return BufferArea<const float>(buffer_, get_width(), area, elem_stride);
  }

  BuffersIterator<float> iterate_with(Span<MemoryBuffer *> inputs);
  BuffersIterator<float> iterate_with(Span<MemoryBuffer *> inputs, const rcti &area);

  /**
   * \brief get the data of this MemoryBuffer
   * \note buffer should already be available in memory
   */
  float *get_buffer()
  {
    return buffer_;
  }

  float *release_ownership_buffer()
  {
    owns_data_ = false;
    return buffer_;
  }

  /**
   * Converts a single elem buffer to a full size buffer (allocates memory for all
   * elements in resolution).
   */
  MemoryBuffer *inflate() const;

  inline void wrap_pixel(int &x, int &y, MemoryBufferExtend extend_x, MemoryBufferExtend extend_y)
  {
    const int w = get_width();
    const int h = get_height();
    x = x - rect_.xmin;
    y = y - rect_.ymin;

    switch (extend_x) {
      case MemoryBufferExtend::Clip:
        break;
      case MemoryBufferExtend::Extend:
        if (x < 0) {
          x = 0;
        }
        if (x >= w) {
          x = w - 1;
        }
        break;
      case MemoryBufferExtend::Repeat:
        x %= w;
        if (x < 0) {
          x += w;
        }
        break;
    }

    switch (extend_y) {
      case MemoryBufferExtend::Clip:
        break;
      case MemoryBufferExtend::Extend:
        if (y < 0) {
          y = 0;
        }
        if (y >= h) {
          y = h - 1;
        }
        break;
      case MemoryBufferExtend::Repeat:
        y %= h;
        if (y < 0) {
          y += h;
        }
        break;
    }

    x = x + rect_.xmin;
    y = y + rect_.ymin;
  }

  inline void wrap_pixel(float &x,
                         float &y,
                         MemoryBufferExtend extend_x,
                         MemoryBufferExtend extend_y) const
  {
    const float w = (float)get_width();
    const float h = (float)get_height();
    x = x - rect_.xmin;
    y = y - rect_.ymin;

    switch (extend_x) {
      case MemoryBufferExtend::Clip:
        break;
      case MemoryBufferExtend::Extend:
        if (x < 0) {
          x = 0.0f;
        }
        if (x >= w) {
          x = w - 1;
        }
        break;
      case MemoryBufferExtend::Repeat:
        x = fmodf(x, w);
        if (x < 0.0f) {
          x += w;
        }
        break;
    }

    switch (extend_y) {
      case MemoryBufferExtend::Clip:
        break;
      case MemoryBufferExtend::Extend:
        if (y < 0) {
          y = 0.0f;
        }
        if (y >= h) {
          y = h - 1;
        }
        break;
      case MemoryBufferExtend::Repeat:
        y = fmodf(y, h);
        if (y < 0.0f) {
          y += h;
        }
        break;
    }

    x = x + rect_.xmin;
    y = y + rect_.ymin;
  }

  /* TODO(manzanilla): to be removed with tiled implementation. For applying #MemoryBufferExtend
   * use #wrap_pixel. */
  inline void read(float *result,
                   int x,
                   int y,
                   MemoryBufferExtend extend_x = MemoryBufferExtend::Clip,
                   MemoryBufferExtend extend_y = MemoryBufferExtend::Clip)
  {
    bool clip_x = (extend_x == MemoryBufferExtend::Clip && (x < rect_.xmin || x >= rect_.xmax));
    bool clip_y = (extend_y == MemoryBufferExtend::Clip && (y < rect_.ymin || y >= rect_.ymax));
    if (clip_x || clip_y) {
      /* clip result outside rect is zero */
      memset(result, 0, num_channels_ * sizeof(float));
    }
    else {
      int u = x;
      int v = y;
      this->wrap_pixel(u, v, extend_x, extend_y);
      const int offset = get_coords_offset(u, v);
      float *buffer = &buffer_[offset];
      memcpy(result, buffer, sizeof(float) * num_channels_);
    }
  }

  /* TODO(manzanilla): to be removed with tiled implementation. */
  inline void read_no_check(float *result,
                            int x,
                            int y,
                            MemoryBufferExtend extend_x = MemoryBufferExtend::Clip,
                            MemoryBufferExtend extend_y = MemoryBufferExtend::Clip)
  {
    int u = x;
    int v = y;

    this->wrap_pixel(u, v, extend_x, extend_y);
    const int offset = get_coords_offset(u, v);

    BLI_assert(offset >= 0);
    BLI_assert(offset < this->buffer_len() * num_channels_);
    BLI_assert(!(extend_x == MemoryBufferExtend::Clip && (u < rect_.xmin || u >= rect_.xmax)) &&
               !(extend_y == MemoryBufferExtend::Clip && (v < rect_.ymin || v >= rect_.ymax)));
    float *buffer = &buffer_[offset];
    memcpy(result, buffer, sizeof(float) * num_channels_);
  }

  void write_pixel(int x, int y, const float color[4]);
  void add_pixel(int x, int y, const float color[4]);
  inline void read_bilinear(float *result,
                            float x,
                            float y,
                            MemoryBufferExtend extend_x = MemoryBufferExtend::Clip,
                            MemoryBufferExtend extend_y = MemoryBufferExtend::Clip) const
  {
    float u = x;
    float v = y;
    this->wrap_pixel(u, v, extend_x, extend_y);
    if ((extend_x != MemoryBufferExtend::Repeat && (u < 0.0f || u >= get_width())) ||
        (extend_y != MemoryBufferExtend::Repeat && (v < 0.0f || v >= get_height()))) {
      copy_vn_fl(result, num_channels_, 0.0f);
      return;
    }
    if (is_a_single_elem_) {
      memcpy(result, buffer_, sizeof(float) * num_channels_);
    }
    else {
      BLI_bilinear_interpolation_wrap_fl(buffer_,
                                         result,
                                         get_width(),
                                         get_height(),
                                         num_channels_,
                                         u,
                                         v,
                                         extend_x == MemoryBufferExtend::Repeat,
                                         extend_y == MemoryBufferExtend::Repeat);
    }
  }

  void readEWA(float *result, const float uv[2], const float derivatives[2][2]);

  /**
   * \brief is this MemoryBuffer a temporarily buffer (based on an area, not on a chunk)
   */
  inline bool is_temporarily() const
  {
    return state_ == MemoryBufferState::Temporary;
  }

  /**
   * \brief Apply a color processor on the given area.
   */
  void apply_processor(ColormanageProcessor &processor, const rcti area);

  void copy_from(const MemoryBuffer *src, const rcti &area);
  void copy_from(const MemoryBuffer *src, const rcti &area, int to_x, int to_y);
  void copy_from(const MemoryBuffer *src,
                 const rcti &area,
                 int channel_offset,
                 int elem_size,
                 int to_channel_offset);
  void copy_from(const MemoryBuffer *src,
                 const rcti &area,
                 int channel_offset,
                 int elem_size,
                 int to_x,
                 int to_y,
                 int to_channel_offset);
  void copy_from(const uchar *src, const rcti &area);
  void copy_from(const uchar *src,
                 const rcti &area,
                 int channel_offset,
                 int elem_size,
                 int elem_stride,
                 int row_stride,
                 int to_channel_offset);
  void copy_from(const uchar *src,
                 const rcti &area,
                 int channel_offset,
                 int elem_size,
                 int elem_stride,
                 int row_stride,
                 int to_x,
                 int to_y,
                 int to_channel_offset);
  void copy_from(const struct ImBuf *src, const rcti &area, bool ensure_linear_space = false);
  void copy_from(const struct ImBuf *src,
                 const rcti &area,
                 int channel_offset,
                 int elem_size,
                 int to_channel_offset,
                 bool ensure_linear_space = false);
  void copy_from(const struct ImBuf *src,
                 const rcti &src_area,
                 int channel_offset,
                 int elem_size,
                 int to_x,
                 int to_y,
                 int to_channel_offset,
                 bool ensure_linear_space = false);

  void fill(const rcti &area, const float *value);
  void fill(const rcti &area, int channel_offset, const float *value, int value_size);
  /**
   * \brief add the content from other_buffer to this MemoryBuffer
   * \param other_buffer: source buffer
   *
   * \note take care when running this on a new buffer since it won't fill in
   *       uninitialized values in areas where the buffers don't overlap.
   */
  void fill_from(const MemoryBuffer &src);

  /**
   * \brief get the rect of this MemoryBuffer
   */
  const rcti &get_rect() const
  {
    return rect_;
  }

  /**
   * \brief get the width of this MemoryBuffer
   */
  const int get_width() const
  {
    return BLI_rcti_size_x(&rect_);
  }

  /**
   * \brief get the height of this MemoryBuffer
   */
  const int get_height() const
  {
    return BLI_rcti_size_y(&rect_);
  }

  /**
   * \brief clear the buffer. Make all pixels black transparent.
   */
  void clear();

  float get_max_value() const;
  float get_max_value(const rcti &rect) const;

 private:
  void set_strides();
  const int buffer_len() const
  {
    return get_memory_width() * get_memory_height();
  }

  void clear_elem(float *out) const
  {
    memset(out, 0, num_channels_ * sizeof(float));
  }

  template<typename T> T get_relative_x(T x) const
  {
    return x - rect_.xmin;
  }

  template<typename T> T get_relative_y(T y) const
  {
    return y - rect_.ymin;
  }

  template<typename T> bool has_coords(T x, T y) const
  {
    return has_x(x) && has_y(y);
  }

  template<typename T> bool has_x(T x) const
  {
    return x >= rect_.xmin && x < rect_.xmax;
  }

  template<typename T> bool has_y(T y) const
  {
    return y >= rect_.ymin && y < rect_.ymax;
  }

  /* Fast `floor(..)` functions. The caller should check result is within buffer bounds.
   * It `ceil(..)` in near cases and when given coordinate
   * is negative and less than buffer rect `min - 1`. */
  int floor_x(float x) const
  {
    return (int)(x + to_positive_x_stride_) - to_positive_x_stride_;
  }

  int floor_y(float y) const
  {
    return (int)(y + to_positive_y_stride_) - to_positive_y_stride_;
  }

  void copy_single_elem_from(const MemoryBuffer *src,
                             int channel_offset,
                             int elem_size,
                             int to_channel_offset);
  void copy_rows_from(const MemoryBuffer *src, const rcti &src_area, int to_x, int to_y);
  void copy_elems_from(const MemoryBuffer *src,
                       const rcti &area,
                       int channel_offset,
                       int elem_size,
                       int to_x,
                       int to_y,
                       int to_channel_offset);

#ifdef WITH_CXX_GUARDEDALLOC
  MEM_CXX_CLASS_ALLOC_FUNCS("COM:MemoryBuffer")
#endif
};

}  // namespace blender::compositor
