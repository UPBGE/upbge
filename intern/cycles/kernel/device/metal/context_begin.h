/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2021-2022 Blender Foundation */

// clang-format off

/* Open the Metal kernel context class
 * Necessary to access resource bindings */
class MetalKernelContext {
  public:
    constant KernelParamsMetal &launch_params_metal;
    constant MetalAncillaries *metal_ancillaries;

    MetalKernelContext(constant KernelParamsMetal &_launch_params_metal, constant MetalAncillaries * _metal_ancillaries)
    : launch_params_metal(_launch_params_metal), metal_ancillaries(_metal_ancillaries)
    {}

    MetalKernelContext(constant KernelParamsMetal &_launch_params_metal)
    : launch_params_metal(_launch_params_metal)
    {}

    /* texture fetch adapter functions */
    typedef uint64_t ccl_gpu_tex_object_2D;
    typedef uint64_t ccl_gpu_tex_object_3D;

    template<typename T>
    inline __attribute__((__always_inline__))
    T ccl_gpu_tex_object_read_2D(ccl_gpu_tex_object_2D tex, float x, float y) const {
      kernel_assert(0);
      return 0;
    }
    template<typename T>
    inline __attribute__((__always_inline__))
    T ccl_gpu_tex_object_read_3D(ccl_gpu_tex_object_3D tex, float x, float y, float z) const {
      kernel_assert(0);
      return 0;
    }

    // texture2d
    template<>
    inline __attribute__((__always_inline__))
    float4 ccl_gpu_tex_object_read_2D(ccl_gpu_tex_object_2D tex, float x, float y) const {
      const uint tid(tex);
      const uint sid(tex >> 32);
      return metal_ancillaries->textures_2d[tid].tex.sample(metal_samplers[sid], float2(x, y));
    }
    template<>
    inline __attribute__((__always_inline__))
    float ccl_gpu_tex_object_read_2D(ccl_gpu_tex_object_2D tex, float x, float y) const {
      const uint tid(tex);
      const uint sid(tex >> 32);
      return metal_ancillaries->textures_2d[tid].tex.sample(metal_samplers[sid], float2(x, y)).x;
    }

    // texture3d
    template<>
    inline __attribute__((__always_inline__))
    float4 ccl_gpu_tex_object_read_3D(ccl_gpu_tex_object_3D tex, float x, float y, float z) const {
      const uint tid(tex);
      const uint sid(tex >> 32);
      return metal_ancillaries->textures_3d[tid].tex.sample(metal_samplers[sid], float3(x, y, z));
    }
    template<>
    inline __attribute__((__always_inline__))
    float ccl_gpu_tex_object_read_3D(ccl_gpu_tex_object_3D tex, float x, float y, float z) const {
      const uint tid(tex);
      const uint sid(tex >> 32);
      return metal_ancillaries->textures_3d[tid].tex.sample(metal_samplers[sid], float3(x, y, z)).x;
    }
#    include "kernel/device/gpu/image.h"

  // clang-format on
