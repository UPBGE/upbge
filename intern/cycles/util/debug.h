/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <cassert>

#include "bvh/params.h"

CCL_NAMESPACE_BEGIN

/* Global storage for all sort of flags used to fine-tune behavior of particular
 * areas for the development purposes, without officially exposing settings to
 * the interface.
 */
class DebugFlags {
 public:
  /* Descriptor of CPU feature-set to be used. */
  struct CPU {
    CPU();

    /* Reset flags to their defaults. */
    void reset();

    /* Flags describing which instructions sets are allowed for use. */
    bool avx2 = true;
    bool sse42 = true;

    /* Check functions to see whether instructions up to the given one
     * are allowed for use.
     */
    bool has_avx2()
    {
      return has_sse42() && avx2;
    }
    bool has_sse42()
    {
      return sse42;
    }

    /* Requested BVH layout.
     *
     * By default the fastest will be used. For debugging the BVH used by other
     * CPUs and GPUs can be selected here instead.
     */
    BVHLayout bvh_layout = BVH_LAYOUT_AUTO;
  };

  /* Descriptor of CUDA feature-set to be used. */
  struct CUDA {
    CUDA();

    /* Reset flags to their defaults. */
    void reset();

    /* Whether adaptive feature based runtime compile is enabled or not.
     * Requires the CUDA Toolkit and only works on Linux at the moment. */
    bool adaptive_compile = false;
  };

  /* Descriptor of HIP feature-set to be used. */
  struct HIP {
    HIP();

    /* Reset flags to their defaults. */
    void reset();

    /* Whether adaptive feature based runtime compile is enabled or not. */
    bool adaptive_compile = false;
  };

  /* Descriptor of OptiX feature-set to be used. */
  struct OptiX {
    OptiX();

    /* Reset flags to their defaults. */
    void reset();

    /* Load OptiX module with debug capabilities. Will lower logging verbosity level, enable
     * validations, and lower optimization level. */
    bool use_debug = false;
  };

  /* Descriptor of Metal feature-set to be used. */
  struct Metal {
    Metal();

    /* Reset flags to their defaults. */
    void reset();

    /* Whether adaptive feature based runtime compile is enabled or not. */
    bool adaptive_compile = false;

    /* Whether local atomic sorting is enabled or not. */
    bool use_local_atomic_sort = true;

    /* Whether nanovdb is enabled or not. */
    bool use_nanovdb = true;

    /* Whether async PSO creation is enabled or not. */
    bool use_async_pso_creation = true;

    /* Whether to use per-component motion interpolation.
     */
    bool use_metalrt_pcmi = true;
  };

  /* Get instance of debug flags registry. */
  static DebugFlags &get()
  {
    static DebugFlags instance;
    return instance;
  }

  /* Reset flags to their defaults. */
  void reset();

  /* Requested CPU flags. */
  CPU cpu;

  /* Requested CUDA flags. */
  CUDA cuda;

  /* Requested OptiX flags. */
  OptiX optix;

  /* Requested HIP flags. */
  HIP hip;

  /* Requested Metal flags. */
  Metal metal;

 private:
  DebugFlags() = default;

 public:
  explicit DebugFlags(const DebugFlags & /*other*/) = delete;
  void operator=(const DebugFlags & /*other*/) = delete;
};

using DebugFlagsRef = DebugFlags &;
using DebugFlagsConstRef = const DebugFlags &;

inline DebugFlags &DebugFlags()
{
  return DebugFlags::get();
}

CCL_NAMESPACE_END
