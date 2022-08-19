/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#pragma once

#include "kernel/sample/jitter.h"
#include "util/hash.h"

CCL_NAMESPACE_BEGIN

/* Pseudo random numbers, uncomment this for debugging correlations. Only run
 * this single threaded on a CPU for repeatable results. */
//#define __DEBUG_CORRELATION__

/* High Dimensional Sobol.
 *
 * Multidimensional sobol with generator matrices. Dimension 0 and 1 are equal
 * to classic Van der Corput and Sobol sequences. */

#ifdef __SOBOL__

/* Skip initial numbers that for some dimensions have clear patterns that
 * don't cover the entire sample space. Ideally we would have a better
 * progressive pattern that doesn't suffer from this problem, because even
 * with this offset some dimensions are quite poor.
 */
#  define SOBOL_SKIP 64

ccl_device uint sobol_dimension(KernelGlobals kg, int index, int dimension)
{
  uint result = 0;
  uint i = index + SOBOL_SKIP;
  for (int j = 0, x; (x = find_first_set(i)); i >>= x) {
    j += x;
    result ^= __float_as_uint(kernel_data_fetch(sample_pattern_lut, 32 * dimension + j - 1));
  }
  return result;
}

#endif /* __SOBOL__ */

ccl_device_forceinline float path_rng_1D(KernelGlobals kg,
                                         uint rng_hash,
                                         int sample,
                                         int dimension)
{
#ifdef __DEBUG_CORRELATION__
  return (float)drand48();
#endif

#ifdef __SOBOL__
  if (kernel_data.integrator.sampling_pattern == SAMPLING_PATTERN_PMJ)
#endif
  {
    return pmj_sample_1D(kg, sample, rng_hash, dimension);
  }

#ifdef __SOBOL__
  /* Sobol sequence value using direction vectors. */
  uint result = sobol_dimension(kg, sample, dimension);
  float r = (float)result * (1.0f / (float)0xFFFFFFFF);

  /* Cranly-Patterson rotation using rng seed */
  float shift;

  /* Hash rng with dimension to solve correlation issues.
   * See T38710, T50116.
   */
  uint tmp_rng = cmj_hash_simple(dimension, rng_hash);
  shift = tmp_rng * (kernel_data.integrator.scrambling_distance / (float)0xFFFFFFFF);

  return r + shift - floorf(r + shift);
#endif
}

ccl_device_forceinline void path_rng_2D(KernelGlobals kg,
                                        uint rng_hash,
                                        int sample,
                                        int dimension,
                                        ccl_private float *fx,
                                        ccl_private float *fy)
{
#ifdef __DEBUG_CORRELATION__
  *fx = (float)drand48();
  *fy = (float)drand48();
  return;
#endif

#ifdef __SOBOL__
  if (kernel_data.integrator.sampling_pattern == SAMPLING_PATTERN_PMJ)
#endif
  {
    pmj_sample_2D(kg, sample, rng_hash, dimension, fx, fy);

    return;
  }

#ifdef __SOBOL__
  /* Sobol. */
  *fx = path_rng_1D(kg, rng_hash, sample, dimension);
  *fy = path_rng_1D(kg, rng_hash, sample, dimension + 1);
#endif
}

/**
 * 1D hash recommended from "Hash Functions for GPU Rendering" JCGT Vol. 9, No. 3, 2020
 * See https://www.shadertoy.com/view/4tXyWN and https://www.shadertoy.com/view/XlGcRh
 * http://www.jcgt.org/published/0009/03/02/paper.pdf
 */
ccl_device_inline uint hash_iqint1(uint n)
{
  n = (n << 13U) ^ n;
  n = n * (n * n * 15731U + 789221U) + 1376312589U;

  return n;
}

/**
 * 2D hash recommended from "Hash Functions for GPU Rendering" JCGT Vol. 9, No. 3, 2020
 * See https://www.shadertoy.com/view/4tXyWN and https://www.shadertoy.com/view/XlGcRh
 * http://www.jcgt.org/published/0009/03/02/paper.pdf
 */
ccl_device_inline uint hash_iqnt2d(const uint x, const uint y)
{
  const uint qx = 1103515245U * ((x >> 1U) ^ (y));
  const uint qy = 1103515245U * ((y >> 1U) ^ (x));
  const uint n = 1103515245U * ((qx) ^ (qy >> 3U));

  return n;
}

ccl_device_inline uint path_rng_hash_init(KernelGlobals kg,
                                          const int sample,
                                          const int x,
                                          const int y)
{
  const uint rng_hash = hash_iqnt2d(x, y) ^ kernel_data.integrator.seed;

#ifdef __DEBUG_CORRELATION__
  srand48(rng_hash + sample);
#else
  (void)sample;
#endif

  return rng_hash;
}

ccl_device_inline bool sample_is_even(int pattern, int sample)
{
  if (pattern == SAMPLING_PATTERN_PMJ) {
    /* See Section 10.2.1, "Progressive Multi-Jittered Sample Sequences", Christensen et al.
     * We can use this to get divide sample sequence into two classes for easier variance
     * estimation. */
    return popcount(uint(sample) & 0xaaaaaaaa) & 1;
  }
  else {
    /* TODO(Stefan): Are there reliable ways of dividing CMJ and Sobol into two classes? */
    return sample & 0x1;
  }
}

CCL_NAMESPACE_END
