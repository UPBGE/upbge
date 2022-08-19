/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#include "util/debug.h"

#include <stdlib.h>

#include "bvh/params.h"

#include "util/log.h"
#include "util/string.h"

CCL_NAMESPACE_BEGIN

DebugFlags::CPU::CPU()
{
  reset();
}

void DebugFlags::CPU::reset()
{
#define STRINGIFY(x) #x
#define CHECK_CPU_FLAGS(flag, env) \
  do { \
    flag = (getenv(env) == NULL); \
    if (!flag) { \
      VLOG_INFO << "Disabling " << STRINGIFY(flag) << " instruction set."; \
    } \
  } while (0)

  CHECK_CPU_FLAGS(avx2, "CYCLES_CPU_NO_AVX2");
  CHECK_CPU_FLAGS(avx, "CYCLES_CPU_NO_AVX");
  CHECK_CPU_FLAGS(sse41, "CYCLES_CPU_NO_SSE41");
  CHECK_CPU_FLAGS(sse3, "CYCLES_CPU_NO_SSE3");
  CHECK_CPU_FLAGS(sse2, "CYCLES_CPU_NO_SSE2");

#undef STRINGIFY
#undef CHECK_CPU_FLAGS

  bvh_layout = BVH_LAYOUT_AUTO;
}

DebugFlags::CUDA::CUDA()
{
  reset();
}

DebugFlags::HIP::HIP()
{
  reset();
}

DebugFlags::Metal::Metal()
{
  reset();
}

void DebugFlags::CUDA::reset()
{
  if (getenv("CYCLES_CUDA_ADAPTIVE_COMPILE") != NULL)
    adaptive_compile = true;
}

void DebugFlags::HIP::reset()
{
  if (getenv("CYCLES_HIP_ADAPTIVE_COMPILE") != NULL)
    adaptive_compile = true;
}

void DebugFlags::Metal::reset()
{
  if (getenv("CYCLES_METAL_ADAPTIVE_COMPILE") != NULL)
    adaptive_compile = true;
}

DebugFlags::OptiX::OptiX()
{
  reset();
}

void DebugFlags::OptiX::reset()
{
  use_debug = false;
}

DebugFlags::DebugFlags()
{
  /* Nothing for now. */
}

void DebugFlags::reset()
{
  cpu.reset();
  cuda.reset();
  optix.reset();
  metal.reset();
}

CCL_NAMESPACE_END
