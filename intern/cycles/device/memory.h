/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* Device Memory
 *
 * Data types for allocating, copying and freeing device memory. */

#include "util/array.h"
#include "util/half.h"
#include "util/string.h"
#include "util/texture.h"
#include "util/types.h"

CCL_NAMESPACE_BEGIN

class Device;
class GPUDevice;
class CUDADevice;
class OptiXDevice;
class HIPDevice;
class HIPRTDevice;
class MetalDevice;
class OneapiDevice;

enum MemoryType {
  MEM_READ_ONLY,
  MEM_READ_WRITE,
  MEM_DEVICE_ONLY,
  MEM_GLOBAL,
  MEM_TEXTURE,
};

/* Supported Data Types */

enum DataType {
  TYPE_UNKNOWN,
  TYPE_UCHAR,
  TYPE_UINT16,
  TYPE_UINT,
  TYPE_INT,
  TYPE_FLOAT,
  TYPE_HALF,
  TYPE_UINT64,
};

static constexpr size_t datatype_size(DataType datatype)
{
  switch (datatype) {
    case TYPE_UNKNOWN:
      return 1;
    case TYPE_UCHAR:
      return sizeof(uchar);
    case TYPE_FLOAT:
      return sizeof(float);
    case TYPE_UINT:
      return sizeof(uint);
    case TYPE_UINT16:
      return sizeof(uint16_t);
    case TYPE_INT:
      return sizeof(int);
    case TYPE_HALF:
      return sizeof(half);
    case TYPE_UINT64:
      return sizeof(uint64_t);
    default:
      return 0;
  }
}

/* Traits for data types */

template<typename T> struct device_type_traits {
  static const DataType data_type = TYPE_UNKNOWN;
  static const size_t num_elements = sizeof(T);
};

template<> struct device_type_traits<uchar> {
  static const DataType data_type = TYPE_UCHAR;
  static const size_t num_elements = 1;
  static_assert(sizeof(uchar) == num_elements * datatype_size(data_type));
};

template<> struct device_type_traits<uchar2> {
  static const DataType data_type = TYPE_UCHAR;
  static const size_t num_elements = 2;
  static_assert(sizeof(uchar2) == num_elements * datatype_size(data_type));
};

template<> struct device_type_traits<uchar3> {
  static const DataType data_type = TYPE_UCHAR;
  static const size_t num_elements = 3;
  static_assert(sizeof(uchar3) == num_elements * datatype_size(data_type));
};

template<> struct device_type_traits<uchar4> {
  static const DataType data_type = TYPE_UCHAR;
  static const size_t num_elements = 4;
  static_assert(sizeof(uchar4) == num_elements * datatype_size(data_type));
};

template<> struct device_type_traits<uint> {
  static const DataType data_type = TYPE_UINT;
  static const size_t num_elements = 1;
  static_assert(sizeof(uint) == num_elements * datatype_size(data_type));
};

template<> struct device_type_traits<uint2> {
  static const DataType data_type = TYPE_UINT;
  static const size_t num_elements = 2;
  static_assert(sizeof(uint2) == num_elements * datatype_size(data_type));
};

template<> struct device_type_traits<uint3> {
  /* uint3 has different size depending on the device, can't use it for interchanging
   * memory between CPU and GPU.
   *
   * Leave body empty to trigger a compile error if used. */
};

template<> struct device_type_traits<uint4> {
  static const DataType data_type = TYPE_UINT;
  static const size_t num_elements = 4;
  static_assert(sizeof(uint4) == num_elements * datatype_size(data_type));
};

template<> struct device_type_traits<int> {
  static const DataType data_type = TYPE_INT;
  static const size_t num_elements = 1;
  static_assert(sizeof(int) == num_elements * datatype_size(data_type));
};

template<> struct device_type_traits<int2> {
  static const DataType data_type = TYPE_INT;
  static const size_t num_elements = 2;
  static_assert(sizeof(int2) == num_elements * datatype_size(data_type));
};

template<> struct device_type_traits<int3> {
  /* int3 has different size depending on the device, can't use it for interchanging
   * memory between CPU and GPU.
   *
   * Leave body empty to trigger a compile error if used. */
};

template<> struct device_type_traits<int4> {
  static const DataType data_type = TYPE_INT;
  static const size_t num_elements = 4;
  static_assert(sizeof(int4) == num_elements * datatype_size(data_type));
};

template<> struct device_type_traits<float> {
  static const DataType data_type = TYPE_FLOAT;
  static const size_t num_elements = 1;
  static_assert(sizeof(float) == num_elements * datatype_size(data_type));
};

template<> struct device_type_traits<float2> {
  static const DataType data_type = TYPE_FLOAT;
  static const size_t num_elements = 2;
  static_assert(sizeof(float2) == num_elements * datatype_size(data_type));
};

template<> struct device_type_traits<float3> {
  /* float3 has different size depending on the device, can't use it for interchanging
   * memory between CPU and GPU.
   *
   * Leave body empty to trigger a compile error if used. */
};

template<> struct device_type_traits<packed_float3> {
  static const DataType data_type = TYPE_FLOAT;
  static const size_t num_elements = 3;
  static_assert(sizeof(packed_float3) == num_elements * datatype_size(data_type));
};

template<> struct device_type_traits<float4> {
  static const DataType data_type = TYPE_FLOAT;
  static const size_t num_elements = 4;
  static_assert(sizeof(float4) == num_elements * datatype_size(data_type));
};

template<> struct device_type_traits<half> {
  static const DataType data_type = TYPE_HALF;
  static const size_t num_elements = 1;
  static_assert(sizeof(half) == num_elements * datatype_size(data_type));
};

template<> struct device_type_traits<ushort4> {
  static const DataType data_type = TYPE_UINT16;
  static const size_t num_elements = 4;
  static_assert(sizeof(ushort4) == num_elements * datatype_size(data_type));
};

template<> struct device_type_traits<uint16_t> {
  static const DataType data_type = TYPE_UINT16;
  static const size_t num_elements = 1;
  static_assert(sizeof(uint16_t) == num_elements * datatype_size(data_type));
};

template<> struct device_type_traits<half4> {
  static const DataType data_type = TYPE_HALF;
  static const size_t num_elements = 4;
  static_assert(sizeof(half4) == num_elements * datatype_size(data_type));
};

template<> struct device_type_traits<uint64_t> {
  static const DataType data_type = TYPE_UINT64;
  static const size_t num_elements = 1;
  static_assert(sizeof(uint64_t) == num_elements * datatype_size(data_type));
};

/* Device Memory
 *
 * Base class for all device memory. This should not be allocated directly,
 * instead the appropriate subclass can be used. */

class device_memory {
 public:
  size_t memory_size()
  {
    return data_size * data_elements * datatype_size(data_type);
  }
  size_t memory_elements_size(const int elements)
  {
    return elements * data_elements * datatype_size(data_type);
  }

  /* Data information. */
  DataType data_type;
  int data_elements;
  size_t data_size;
  size_t device_size;
  size_t data_width;
  size_t data_height;
  MemoryType type;
  const char *name;
  string name_storage;

  /* Pointers. */
  Device *device;
  device_ptr device_pointer;
  void *host_pointer;
  void *shared_pointer;
  /* reference counter for shared_pointer */
  int shared_counter;
  bool move_to_host = false;

  virtual ~device_memory();

  void swap_device(Device *new_device, const size_t new_device_size, device_ptr new_device_ptr);
  void restore_device();

  bool is_resident(Device *sub_device) const;
  bool is_shared(Device *sub_device) const;

  /* No copying and allowed.
   *
   * This is because device implementation might need to register device memory in an allocation
   * map of some sort and use pointer as a key to identify blocks. Moving data from one place to
   * another bypassing device allocation routines will make those maps hard to maintain. */
  device_memory(const device_memory &) = delete;
  device_memory(device_memory &&other) noexcept = delete;
  device_memory &operator=(const device_memory &) = delete;
  device_memory &operator=(device_memory &&) = delete;

 protected:
  friend class Device;
  friend class GPUDevice;
  friend class CUDADevice;
  friend class OptiXDevice;
  friend class HIPDevice;
  friend class HIPRTDevice;
  friend class MetalDevice;
  friend class OneapiDevice;

  /* Only create through subclasses. */
  device_memory(Device *device, const char *name, MemoryType type);

  /* Host allocation on the device. All host_pointer memory should be
   * allocated with these functions, for devices that support using
   * the same pointer for host and device. */
  void *host_alloc(const size_t size);

  /* Device memory allocation and copying. */
  void device_alloc();
  void device_copy_to();
  void device_move_to_host();
  void device_copy_from(const size_t y, const size_t w, size_t h, const size_t elem);
  void device_zero();

  /* Memory can only be freed on host and device together. */
  void host_and_device_free();

  bool device_is_cpu();

  device_ptr original_device_ptr;
  size_t original_device_size;
  Device *original_device;
  bool need_realloc_;
  bool modified;
};

/* Device Only Memory
 *
 * Working memory only needed by the device, with no corresponding allocation
 * on the host. Only used internally in the device implementations. */

template<typename T> class device_only_memory : public device_memory {
 public:
  device_only_memory(Device *device, const char *name, bool allow_host_memory_fallback = false)
      : device_memory(device, name, allow_host_memory_fallback ? MEM_READ_WRITE : MEM_DEVICE_ONLY)
  {
    data_type = device_type_traits<T>::data_type;
    data_elements = max(device_type_traits<T>::num_elements, size_t(1));
  }

  device_only_memory(device_only_memory &&other) noexcept : device_memory(std::move(other)) {}

  ~device_only_memory() override
  {
    free();
  }

  void alloc_to_device(const size_t num, bool shrink_to_fit = true)
  {
    size_t new_size = num;
    bool reallocate;

    if (shrink_to_fit) {
      reallocate = (data_size != new_size);
    }
    else {
      reallocate = (data_size < new_size);
    }

    if (reallocate) {
      host_and_device_free();
      data_size = new_size;
      device_alloc();
    }
  }

  void free()
  {
    host_and_device_free();
    data_size = 0;
  }

  void zero_to_device()
  {
    device_zero();
  }
};

/* Device Vector
 *
 * Data vector to exchange data between host and device. Memory will be
 * allocated on the host first with alloc() and resize, and then filled
 * in and copied to the device with copy_to_device(). Or alternatively
 * allocated and set to zero on the device with zero_to_device().
 *
 * When using memory type MEM_GLOBAL, a pointer to this memory will be
 * automatically attached to kernel globals, using the provided name
 * matching an entry in kernel/data_arrays.h. */

template<typename T> class device_vector : public device_memory {
 public:
  device_vector(Device *device, const char *name, MemoryType type)
      : device_memory(device, name, type)
  {
    data_type = device_type_traits<T>::data_type;
    data_elements = device_type_traits<T>::num_elements;
    modified = true;
    need_realloc_ = true;

    assert(data_elements > 0);
  }

  ~device_vector() override
  {
    free();
  }

  /* Host memory allocation. */
  T *alloc(const size_t width, const size_t height = 0)
  {
    size_t new_size = size(width, height);

    if (new_size != data_size) {
      host_and_device_free();
      host_pointer = host_alloc(sizeof(T) * new_size);
      modified = true;
      assert(device_pointer == 0);
    }

    data_size = new_size;
    data_width = width;
    data_height = height;

    return data();
  }

  /* Host memory resize. Only use this if the original data needs to be
   * preserved or memory needs to be initialized, it is faster to call
   * alloc() if it can be discarded. */
  T *resize(const size_t width, const size_t height = 0)
  {
    size_t new_size = size(width, height);

    if (new_size != data_size) {
      void *new_ptr = host_alloc(sizeof(T) * new_size);

      if (new_ptr) {
        size_t min_size = (new_size < data_size) ? new_size : data_size;
        for (size_t i = 0; i < min_size; i++) {
          ((T *)new_ptr)[i] = ((T *)host_pointer)[i];
        }
        for (size_t i = data_size; i < new_size; i++) {
          ((T *)new_ptr)[i] = T();
        }
      }

      host_and_device_free();
      host_pointer = new_ptr;
      assert(device_pointer == 0);
    }

    data_size = new_size;
    data_width = width;
    data_height = height;

    return data();
  }

  /* Take over data from an existing array. */
  void steal_data(array<T> &from)
  {
    host_and_device_free();

    data_size = from.size();
    data_width = 0;
    data_height = 0;
    host_pointer = from.steal_pointer();
    assert(device_pointer == 0);
  }

  /* Free device and host memory. */
  void free()
  {
    host_and_device_free();

    data_size = 0;
    data_width = 0;
    data_height = 0;
    host_pointer = 0;
    modified = true;
    need_realloc_ = true;
    assert(device_pointer == 0);
  }

  void free_if_need_realloc(bool force_free)
  {
    if (need_realloc_ || force_free) {
      free();
    }
  }

  bool is_modified() const
  {
    return modified;
  }

  bool need_realloc()
  {
    return need_realloc_;
  }

  void tag_modified()
  {
    modified = true;
  }

  void tag_realloc()
  {
    need_realloc_ = true;
    tag_modified();
  }

  size_t size() const
  {
    return data_size;
  }

  T *data()
  {
    return (T *)host_pointer;
  }

  const T *data() const
  {
    return (T *)host_pointer;
  }

  T &operator[](size_t i)
  {
    assert(i < data_size);
    return data()[i];
  }

  void copy_to_device()
  {
    if (data_size != 0) {
      device_copy_to();
    }
  }

  void copy_to_device_if_modified()
  {
    if (!modified) {
      return;
    }

    copy_to_device();
  }

  void clear_modified()
  {
    modified = false;
    need_realloc_ = false;
  }

  void copy_from_device()
  {
    device_copy_from(0, data_width, (data_height == 0) ? 1 : data_height, sizeof(T));
  }

  void copy_from_device(const size_t y, const size_t w, size_t h)
  {
    device_copy_from(y, w, h, sizeof(T));
  }

  void zero_to_device()
  {
    device_zero();
  }

 protected:
  size_t size(const size_t width, const size_t height)
  {
    return width * ((height == 0) ? 1 : height);
  }
};

/* Device Sub Memory
 *
 * Pointer into existing memory. It is not allocated separately, but created
 * from an already allocated base memory. It is freed automatically when it
 * goes out of scope, which should happen before base memory is freed.
 *
 * NOTE: some devices require offset and size of the sub_ptr to be properly
 * aligned to device->mem_address_alingment(). */

class device_sub_ptr {
 public:
  device_sub_ptr(device_memory &mem, const size_t offset, const size_t size);
  ~device_sub_ptr();

  device_ptr operator*() const
  {
    return ptr;
  }

 protected:
  /* No copying. */
  device_sub_ptr &operator=(const device_sub_ptr &);

  Device *device;
  device_ptr ptr;
};

/* Device Texture
 *
 * 2D or 3D image texture memory. */

class device_texture : public device_memory {
 public:
  device_texture(Device *device,
                 const char *name,
                 const uint slot,
                 ImageDataType image_data_type,
                 InterpolationType interpolation,
                 ExtensionType extension);
  ~device_texture() override;

  void *alloc(const size_t width, const size_t height);
  void copy_to_device();

  uint slot = 0;
  TextureInfo info;

 protected:
  size_t size(const size_t width, const size_t height)
  {
    return width * ((height == 0) ? 1 : height);
  }
};

CCL_NAMESPACE_END
