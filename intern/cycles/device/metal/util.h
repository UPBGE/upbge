/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2021-2022 Blender Foundation */

#pragma once

#ifdef WITH_METAL

#  include <Metal/Metal.h>
#  include <string>

#  include "device/metal/device.h"
#  include "device/metal/kernel.h"
#  include "device/queue.h"

#  include "util/thread.h"

#  define metal_printf VLOG(4) << string_printf

CCL_NAMESPACE_BEGIN

enum MetalGPUVendor {
  METAL_GPU_UNKNOWN = 0,
  METAL_GPU_APPLE = 1,
  METAL_GPU_AMD = 2,
  METAL_GPU_INTEL = 3,
};

enum AppleGPUArchitecture {
  APPLE_UNKNOWN,
  APPLE_M1,
  APPLE_M2,
};

/* Contains static Metal helper functions. */
struct MetalInfo {
  static vector<id<MTLDevice>> const &get_usable_devices();
  static int get_apple_gpu_core_count(id<MTLDevice> device);
  static MetalGPUVendor get_device_vendor(id<MTLDevice> device);
  static AppleGPUArchitecture get_apple_gpu_architecture(id<MTLDevice> device);
  static int optimal_sort_partition_elements(id<MTLDevice> device);
  static string get_device_name(id<MTLDevice> device);
};

/* Pool of MTLBuffers whose lifetime is linked to a single MTLCommandBuffer */
class MetalBufferPool {
  struct MetalBufferListEntry {
    MetalBufferListEntry(id<MTLBuffer> buffer, id<MTLCommandBuffer> command_buffer)
        : buffer(buffer), command_buffer(command_buffer)
    {
    }

    MetalBufferListEntry() = delete;

    id<MTLBuffer> buffer;
    id<MTLCommandBuffer> command_buffer;
  };
  std::vector<MetalBufferListEntry> buffer_free_list;
  std::vector<MetalBufferListEntry> buffer_in_use_list;
  thread_mutex buffer_mutex;
  size_t total_temp_mem_size = 0;

 public:
  MetalBufferPool() = default;
  ~MetalBufferPool();

  id<MTLBuffer> get_buffer(id<MTLDevice> device,
                           id<MTLCommandBuffer> command_buffer,
                           NSUInteger length,
                           MTLResourceOptions options,
                           const void *pointer,
                           Stats &stats);
  void process_command_buffer_completion(id<MTLCommandBuffer> command_buffer);
};

CCL_NAMESPACE_END

#endif /* WITH_METAL */
