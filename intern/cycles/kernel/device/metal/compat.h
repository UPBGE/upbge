/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#define __KERNEL_GPU__
#define __KERNEL_METAL__
#define CCL_NAMESPACE_BEGIN
#define CCL_NAMESPACE_END

#ifndef ATTR_FALLTHROUGH
#  define ATTR_FALLTHROUGH
#endif

#include <metal_atomic>
#include <metal_pack>
#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

#ifdef __METALRT__
using namespace metal::raytracing;
#endif

#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wuninitialized"
#pragma clang diagnostic ignored "-Wc++17-extensions"
#pragma clang diagnostic ignored "-Wmacro-redefined"

/* Qualifiers */

#define ccl_device
#define ccl_device_inline ccl_device __attribute__((always_inline))
#define ccl_device_forceinline ccl_device __attribute__((always_inline))
#if defined(__KERNEL_METAL_APPLE__)
#  define ccl_device_noinline ccl_device
#else
#  define ccl_device_noinline ccl_device __attribute__((noinline))
#endif

#define ccl_device_extern extern "C"
#define ccl_device_noinline_cpu ccl_device
#define ccl_device_inline_method ccl_device
#define ccl_device_template_spec template<> ccl_device_inline
#define ccl_global device
#define ccl_inline_constant static constant constexpr
#define ccl_device_constant constant
#define ccl_static_constexpr static constant constexpr
#define ccl_constant constant
#define ccl_gpu_shared threadgroup
#define ccl_private thread
#ifdef __METALRT__
#  define ccl_ray_data ray_data
#else
#  define ccl_ray_data ccl_private
#endif
#define ccl_may_alias
#define ccl_restrict __restrict
#define ccl_align(n) alignas(n)
#define ccl_optional_struct_init

/* No assert supported for Metal */

#define kernel_assert(cond)

#define offsetof(t, d) __builtin_offsetof(t, d)

#define ccl_gpu_global_id_x() metal_global_id
#define ccl_gpu_warp_size simdgroup_size
#define ccl_gpu_thread_idx_x simd_group_index
#define ccl_gpu_thread_mask(thread_warp) uint64_t((1ull << thread_warp) - 1)

#define ccl_gpu_ballot(predicate) ((uint64_t)((simd_vote::vote_t)simd_ballot(predicate)))
#define ccl_gpu_syncthreads() threadgroup_barrier(mem_flags::mem_threadgroup);

// clang-format off

/* kernel.h adapters */

#define ccl_gpu_kernel(block_num_threads, thread_num_registers)
#define ccl_gpu_kernel_threads(block_num_threads)

/* Convert a comma-separated list into a semicolon-separated list
 * (so that we can generate a struct based on kernel entry-point parameters). */
#define FN0()
#define FN1(p1) p1;
#define FN2(p1, p2) p1; p2;
#define FN3(p1, p2, p3) p1; p2; p3;
#define FN4(p1, p2, p3, p4) p1; p2; p3; p4;
#define FN5(p1, p2, p3, p4, p5) p1; p2; p3; p4; p5;
#define FN6(p1, p2, p3, p4, p5, p6) p1; p2; p3; p4; p5; p6;
#define FN7(p1, p2, p3, p4, p5, p6, p7) p1; p2; p3; p4; p5; p6; p7;
#define FN8(p1, p2, p3, p4, p5, p6, p7, p8) p1; p2; p3; p4; p5; p6; p7; p8;
#define FN9(p1, p2, p3, p4, p5, p6, p7, p8, p9) p1; p2; p3; p4; p5; p6; p7; p8; p9;
#define FN10(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10) p1; p2; p3; p4; p5; p6; p7; p8; p9; p10;
#define FN11(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11) p1; p2; p3; p4; p5; p6; p7; p8; p9; p10; p11;
#define FN12(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12) p1; p2; p3; p4; p5; p6; p7; p8; p9; p10; p11; p12;
#define FN13(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13) p1; p2; p3; p4; p5; p6; p7; p8; p9; p10; p11; p12; p13;
#define FN14(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14) p1; p2; p3; p4; p5; p6; p7; p8; p9; p10; p11; p12; p13; p14;
#define FN15(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15) p1; p2; p3; p4; p5; p6; p7; p8; p9; p10; p11; p12; p13; p14; p15;
#define FN16(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15, p16) p1; p2; p3; p4; p5; p6; p7; p8; p9; p10; p11; p12; p13; p14; p15; p16;
#define FN17(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15, p16, p17) p1; p2; p3; p4; p5; p6; p7; p8; p9; p10; p11; p12; p13; p14; p15; p16; p17;
#define FN18(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15, p16, p17, p18) p1; p2; p3; p4; p5; p6; p7; p8; p9; p10; p11; p12; p13; p14; p15; p16; p17; p18;
#define FN19(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15, p16, p17, p18, p19) p1; p2; p3; p4; p5; p6; p7; p8; p9; p10; p11; p12; p13; p14; p15; p16; p17; p18; p19;
#define FN20(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15, p16, p17, p18, p19, p20) p1; p2; p3; p4; p5; p6; p7; p8; p9; p10; p11; p12; p13; p14; p15; p16; p17; p18; p19; p20;
#define GET_LAST_ARG(p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15, p16, p17, p18, p19, p20, ...) p20
#define PARAMS_MAKER(...) GET_LAST_ARG(__VA_ARGS__, FN20, FN19, FN18, FN17, FN16, FN15, FN14, FN13, FN12, FN11, FN10, FN9, FN8, FN7, FN6, FN5, FN4, FN3, FN2, FN1, FN0)

/* Generate a struct containing the entry-point parameters and a "run"
 * method which can access them implicitly via this-> */

#ifdef __METAL_GLOBAL_BUILTINS__

#define ccl_gpu_kernel_signature(name, ...) \
struct kernel_gpu_##name \
{ \
  PARAMS_MAKER(__VA_ARGS__)(__VA_ARGS__) \
  void run(thread MetalKernelContext& context, \
           threadgroup atomic_int *threadgroup_array) ccl_global const; \
}; \
kernel void cycles_metal_##name(device const kernel_gpu_##name *params_struct, \
                                constant KernelParamsMetal &ccl_restrict   _launch_params_metal, \
                                constant MetalAncillaries *_metal_ancillaries, \
                                threadgroup atomic_int *threadgroup_array[[ threadgroup(0) ]]) { \
  MetalKernelContext context(_launch_params_metal, _metal_ancillaries); \
  params_struct->run(context, threadgroup_array); \
} \
void kernel_gpu_##name::run(thread MetalKernelContext& context, \
                  threadgroup atomic_int *threadgroup_array) ccl_global const

#else

/* On macOS versions before 14.x, builtin constants (e.g. metal_global_id) must
 * be accessed through attributed entry-point parameters. */

#define ccl_gpu_kernel_signature(name, ...) \
struct kernel_gpu_##name \
{ \
  PARAMS_MAKER(__VA_ARGS__)(__VA_ARGS__) \
  void run(thread MetalKernelContext& context, \
           threadgroup atomic_int *threadgroup_array, \
           const uint metal_global_id, \
           const ushort metal_local_id, \
           const ushort metal_local_size, \
           const uint metal_grid_id, \
           uint simdgroup_size, \
           uint simd_lane_index, \
           uint simd_group_index, \
           uint num_simd_groups) ccl_global const; \
}; \
kernel void cycles_metal_##name(device const kernel_gpu_##name *params_struct, \
                                constant KernelParamsMetal &ccl_restrict   _launch_params_metal, \
                                constant MetalAncillaries *_metal_ancillaries, \
                                threadgroup atomic_int *threadgroup_array[[ threadgroup(0) ]], \
                                const uint metal_global_id [[thread_position_in_grid]], \
                                const ushort metal_local_id   [[thread_position_in_threadgroup]], \
                                const ushort metal_local_size [[threads_per_threadgroup]], \
                                const uint metal_grid_id    [[threadgroup_position_in_grid]], \
                                uint simdgroup_size [[threads_per_simdgroup]], \
                                uint simd_lane_index [[thread_index_in_simdgroup]], \
                                uint simd_group_index [[simdgroup_index_in_threadgroup]], \
                                uint num_simd_groups [[simdgroups_per_threadgroup]]) { \
  MetalKernelContext context(_launch_params_metal, _metal_ancillaries); \
  params_struct->run(context, threadgroup_array, metal_global_id, metal_local_id, metal_local_size, metal_grid_id, simdgroup_size, simd_lane_index, simd_group_index, num_simd_groups); \
} \
void kernel_gpu_##name::run(thread MetalKernelContext& context, \
                  threadgroup atomic_int *threadgroup_array, \
                  const uint metal_global_id, \
                  const ushort metal_local_id, \
                  const ushort metal_local_size, \
                  const uint metal_grid_id, \
                  uint simdgroup_size, \
                  uint simd_lane_index, \
                  uint simd_group_index, \
                  uint num_simd_groups) ccl_global const

#endif /* __METAL_GLOBAL_BUILTINS__ */

#define ccl_gpu_kernel_postfix
#define ccl_gpu_kernel_call(x) context.x
#define ccl_gpu_kernel_within_bounds(i,n) true

/* define a function object where "func" is the lambda body, and additional parameters are used to specify captured state. */
#define ccl_gpu_kernel_lambda(func, ...) \
  struct KernelLambda \
  { \
    KernelLambda(ccl_private MetalKernelContext &_context) : context(_context) {} \
    ccl_private MetalKernelContext &context; \
    __VA_ARGS__; \
    int operator()(const int state) const { return (func); } \
  } ccl_gpu_kernel_lambda_pass(context)

// clang-format on

/* make_type definitions with Metal style element initializers */
ccl_device_forceinline float2 make_float2(const float x, const float y)
{
  return float2(x, y);
}

ccl_device_forceinline float3 make_float3(const float x, const float y, const float z)
{
  return float3(x, y, z);
}

ccl_device_forceinline float4 make_float4(const float x,
                                          const float y,
                                          const float z,
                                          const float w)
{
  return float4(x, y, z, w);
}

ccl_device_forceinline int2 make_int2(const int x, const int y)
{
  return int2(x, y);
}

ccl_device_forceinline int3 make_int3(const int x, const int y, const int z)
{
  return int3(x, y, z);
}

ccl_device_forceinline int4 make_int4(const int x, const int y, const int z, const int w)
{
  return int4(x, y, z, w);
}

ccl_device_forceinline uint2 make_uint2(const uint x, const uint y)
{
  return uint2(x, y);
}

ccl_device_forceinline uint3 make_uint3(const uint x, const uint y, const uint z)
{
  return uint3(x, y, z);
}

ccl_device_forceinline uint4 make_uint4(const uint x, const uint y, const uint z, const uint w)
{
  return uint4(x, y, z, w);
}

ccl_device_forceinline uchar4 make_uchar4(const uchar x,
                                          const uchar y,
                                          const uchar z,
                                          const uchar w)
{
  return uchar4(x, y, z, w);
}

/* Math functions */

#define __uint_as_float(x) as_type<float>(x)
#define __float_as_uint(x) as_type<uint>(x)
#define __int_as_float(x) as_type<float>(x)
#define __float_as_int(x) as_type<int>(x)
#define __float2half(x) half(x)
#define powf(x, y) pow(float(x), float(y))
#define fabsf(x) fabs(float(x))
#define copysignf(x, y) copysign(float(x), float(y))
#define asinf(x) asin(float(x))
#define acosf(x) acos(float(x))
#define atanf(x) atan(float(x))
#define floorf(x) floor(float(x))
#define ceilf(x) ceil(float(x))
#define hypotf(x, y) hypot(float(x), float(y))
#define atan2f(x, y) atan2(float(x), float(y))
#define fmaxf(x, y) fmax(float(x), float(y))
#define fminf(x, y) fmin(float(x), float(y))
#define fmodf(x, y) fmod(float(x), float(y))
#define sinhf(x) sinh(float(x))
#define coshf(x) cosh(float(x))
#define tanhf(x) tanh(float(x))
#define saturatef(x) saturate(float(x))

/* Use native functions with possibly lower precision for performance,
 * no issues found so far. */
#define trigmode fast
#define sinf(x) trigmode::sin(float(x))
#define cosf(x) trigmode::cos(float(x))
#define tanf(x) trigmode::tan(float(x))
#define expf(x) trigmode::exp(float(x))
#define sqrtf(x) trigmode::sqrt(float(x))
#define logf(x) trigmode::log(float(x))

#define __device__

#ifdef __METALRT__

#  if defined(__METALRT_MOTION__)
#    define METALRT_TAGS instancing, instance_motion, primitive_motion
#    define METALRT_BLAS_TAGS , primitive_motion
#  else
#    define METALRT_TAGS instancing
#    define METALRT_BLAS_TAGS
#  endif /* __METALRT_MOTION__ */

#  if defined(__METALRT_EXTENDED_LIMITS__)
#    define METALRT_LIMITS , extended_limits
#  else
#    define METALRT_LIMITS
#  endif /* __METALRT_MOTION__ */

typedef acceleration_structure<METALRT_TAGS> metalrt_as_type;
typedef intersection_function_table<triangle_data, curve_data, METALRT_TAGS METALRT_LIMITS>
    metalrt_ift_type;
typedef metal::raytracing::intersector<triangle_data, curve_data, METALRT_TAGS METALRT_LIMITS>
    metalrt_intersector_type;
#  if defined(__METALRT_MOTION__)
typedef acceleration_structure<primitive_motion> metalrt_blas_as_type;
typedef intersection_function_table<triangle_data, curve_data, primitive_motion METALRT_LIMITS>
    metalrt_blas_ift_type;
typedef metal::raytracing::intersector<triangle_data, curve_data, primitive_motion METALRT_LIMITS>
    metalrt_blas_intersector_type;
#  else
typedef acceleration_structure<> metalrt_blas_as_type;
typedef intersection_function_table<triangle_data, curve_data METALRT_LIMITS>
    metalrt_blas_ift_type;
typedef metal::raytracing::intersector<triangle_data, curve_data METALRT_LIMITS>
    metalrt_blas_intersector_type;
#  endif

#endif /* __METALRT__ */

/* texture bindings and sampler setup */

/* TextureParamsMetal is reinterpreted as Texture2DParamsMetal. */
struct TextureParamsMetal {
  uint64_t tex;
};
struct Texture2DParamsMetal {
  texture2d<float, access::sample> tex;
};

#ifdef __METALRT__
struct MetalRTBlasWrapper {
  metalrt_blas_as_type blas;
};
#endif

/* Additional Metal-specific resources which aren't encoded in KernelData.
 * IMPORTANT: If this layout changes, ANCILLARY_SLOT_COUNT and the host-side encoding must change
 * to match. */
struct MetalAncillaries {
  device TextureParamsMetal *textures;

#ifdef __METALRT__
  metalrt_as_type accel_struct;
  constant MetalRTBlasWrapper *blas_accel_structs;
  metalrt_ift_type ift_default;
  metalrt_ift_type ift_shadow;
  metalrt_ift_type ift_shadow_all;
  metalrt_ift_type ift_volume;
  metalrt_blas_ift_type ift_local;
  metalrt_ift_type ift_local_mblur;
  metalrt_blas_ift_type ift_local_single_hit;
  metalrt_ift_type ift_local_single_hit_mblur;
#endif
};

#include "util/half.h"
#include "util/types.h"

enum SamplerType {
  SamplerFilterNearest_AddressRepeat,
  SamplerFilterNearest_AddressClampEdge,
  SamplerFilterNearest_AddressClampZero,
  SamplerFilterNearest_AddressMirroredRepeat,

  SamplerFilterLinear_AddressRepeat,
  SamplerFilterLinear_AddressClampEdge,
  SamplerFilterLinear_AddressClampZero,
  SamplerFilterLinear_AddressMirroredRepeat,

  SamplerCount
};

constexpr constant array<sampler, SamplerCount> metal_samplers = {
    sampler(address::repeat, filter::nearest),
    sampler(address::clamp_to_edge, filter::nearest),
    sampler(address::clamp_to_zero, filter::nearest),
    sampler(address::mirrored_repeat, filter::nearest),
    sampler(address::repeat, filter::linear),
    sampler(address::clamp_to_edge, filter::linear),
    sampler(address::clamp_to_zero, filter::linear),
    sampler(address::mirrored_repeat, filter::linear),
};

#ifdef __METAL_GLOBAL_BUILTINS__
const uint metal_global_id [[thread_position_in_grid]];
const ushort metal_local_id [[thread_position_in_threadgroup]];
const ushort metal_local_size [[threads_per_threadgroup]];
const uint metal_grid_id [[threadgroup_position_in_grid]];
const uint simdgroup_size [[threads_per_simdgroup]];
const uint simd_lane_index [[thread_index_in_simdgroup]];
const uint simd_group_index [[simdgroup_index_in_threadgroup]];
const uint num_simd_groups [[simdgroups_per_threadgroup]];
#endif /* __METAL_GLOBAL_BUILTINS__ */
