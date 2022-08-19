/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

/* Device data taken from CUDA occupancy calculator.
 *
 * Terminology
 * - CUDA GPUs have multiple streaming multiprocessors
 * - Each multiprocessor executes multiple thread blocks
 * - Each thread block contains a number of threads, also known as the block size
 * - Multiprocessors have a fixed number of registers, and the amount of registers
 *   used by each threads limits the number of threads per block.
 */

/* 3.0 and 3.5 */
#if __CUDA_ARCH__ == 300 || __CUDA_ARCH__ == 350
#  define GPU_MULTIPRESSOR_MAX_REGISTERS 65536
#  define GPU_MULTIPROCESSOR_MAX_BLOCKS 16
#  define GPU_BLOCK_MAX_THREADS 1024
#  define GPU_THREAD_MAX_REGISTERS 63

/* tunable parameters */
#  define GPU_KERNEL_BLOCK_NUM_THREADS 256
#  define GPU_KERNEL_MAX_REGISTERS 63

/* 3.2 */
#elif __CUDA_ARCH__ == 320
#  define GPU_MULTIPRESSOR_MAX_REGISTERS 32768
#  define GPU_MULTIPROCESSOR_MAX_BLOCKS 16
#  define GPU_BLOCK_MAX_THREADS 1024
#  define GPU_THREAD_MAX_REGISTERS 63

/* tunable parameters */
#  define GPU_KERNEL_BLOCK_NUM_THREADS 256
#  define GPU_KERNEL_MAX_REGISTERS 63

/* 3.7 */
#elif __CUDA_ARCH__ == 370
#  define GPU_MULTIPRESSOR_MAX_REGISTERS 65536
#  define GPU_MULTIPROCESSOR_MAX_BLOCKS 16
#  define GPU_BLOCK_MAX_THREADS 1024
#  define GPU_THREAD_MAX_REGISTERS 255

/* tunable parameters */
#  define GPU_KERNEL_BLOCK_NUM_THREADS 256
#  define GPU_KERNEL_MAX_REGISTERS 63

/* 5.x, 6.x */
#elif __CUDA_ARCH__ <= 699
#  define GPU_MULTIPRESSOR_MAX_REGISTERS 65536
#  define GPU_MULTIPROCESSOR_MAX_BLOCKS 32
#  define GPU_BLOCK_MAX_THREADS 1024
#  define GPU_THREAD_MAX_REGISTERS 255

/* tunable parameters */
#  define GPU_KERNEL_BLOCK_NUM_THREADS 256
/* CUDA 9.0 seems to cause slowdowns on high-end Pascal cards unless we increase the number of
 * registers */
#  if __CUDACC_VER_MAJOR__ >= 9 && __CUDA_ARCH__ >= 600
#    define GPU_KERNEL_MAX_REGISTERS 64
#  else
#    define GPU_KERNEL_MAX_REGISTERS 48
#  endif

/* 7.x, 8.x */
#elif __CUDA_ARCH__ <= 899
#  define GPU_MULTIPRESSOR_MAX_REGISTERS 65536
#  define GPU_MULTIPROCESSOR_MAX_BLOCKS 32
#  define GPU_BLOCK_MAX_THREADS 1024
#  define GPU_THREAD_MAX_REGISTERS 255

/* tunable parameters */
#  define GPU_KERNEL_BLOCK_NUM_THREADS 512
#  define GPU_KERNEL_MAX_REGISTERS 96

/* unknown architecture */
#else
#  error "Unknown or unsupported CUDA architecture, can't determine launch bounds"
#endif

/* Compute number of threads per block and minimum blocks per multiprocessor
 * given the maximum number of registers per thread. */
#define ccl_gpu_kernel(block_num_threads, thread_num_registers) \
  extern "C" __global__ void __launch_bounds__(block_num_threads, \
                                               GPU_MULTIPRESSOR_MAX_REGISTERS / \
                                                   (block_num_threads * thread_num_registers))

#define ccl_gpu_kernel_threads(block_num_threads) \
  extern "C" __global__ void __launch_bounds__(block_num_threads)

#define ccl_gpu_kernel_signature(name, ...) kernel_gpu_##name(__VA_ARGS__)
#define ccl_gpu_kernel_postfix

#define ccl_gpu_kernel_call(x) x

/* Define a function object where "func" is the lambda body, and additional parameters are used to
 * specify captured state  */
#define ccl_gpu_kernel_lambda(func, ...) \
  struct KernelLambda { \
    __VA_ARGS__; \
    __device__ int operator()(const int state) \
    { \
      return (func); \
    } \
  } ccl_gpu_kernel_lambda_pass

/* sanity checks */

#if GPU_KERNEL_BLOCK_NUM_THREADS > GPU_BLOCK_MAX_THREADS
#  error "Maximum number of threads per block exceeded"
#endif

#if GPU_MULTIPRESSOR_MAX_REGISTERS / (GPU_KERNEL_BLOCK_NUM_THREADS * GPU_KERNEL_MAX_REGISTERS) > \
    GPU_MULTIPROCESSOR_MAX_BLOCKS
#  error "Maximum number of blocks per multiprocessor exceeded"
#endif

#if GPU_KERNEL_MAX_REGISTERS > GPU_THREAD_MAX_REGISTERS
#  error "Maximum number of registers per thread exceeded"
#endif
