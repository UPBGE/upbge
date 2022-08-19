/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#pragma once

#include "kernel/integrator/state.h"

#include "device/cpu/kernel_thread_globals.h"
#include "device/queue.h"

#include "integrator/path_trace_work.h"

#include "util/vector.h"

CCL_NAMESPACE_BEGIN

struct KernelWorkTile;
struct KernelGlobalsCPU;

class CPUKernels;

/* Implementation of PathTraceWork which schedules work on to queues pixel-by-pixel,
 * for CPU devices.
 *
 * NOTE: For the CPU rendering there are assumptions about TBB arena size and number of concurrent
 * queues on the render device which makes this work be only usable on CPU. */
class PathTraceWorkCPU : public PathTraceWork {
 public:
  PathTraceWorkCPU(Device *device,
                   Film *film,
                   DeviceScene *device_scene,
                   bool *cancel_requested_flag);

  virtual void init_execution() override;

  virtual void render_samples(RenderStatistics &statistics,
                              int start_sample,
                              int samples_num,
                              int sample_offset) override;

  virtual void copy_to_display(PathTraceDisplay *display,
                               PassMode pass_mode,
                               int num_samples) override;
  virtual void destroy_gpu_resources(PathTraceDisplay *display) override;

  virtual bool copy_render_buffers_from_device() override;
  virtual bool copy_render_buffers_to_device() override;
  virtual bool zero_render_buffers() override;

  virtual int adaptive_sampling_converge_filter_count_active(float threshold, bool reset) override;
  virtual void cryptomatte_postproces() override;

 protected:
  /* Core path tracing routine. Renders given work time on the given queue. */
  void render_samples_full_pipeline(KernelGlobalsCPU *kernel_globals,
                                    const KernelWorkTile &work_tile,
                                    const int samples_num);

  /* CPU kernels. */
  const CPUKernels &kernels_;

  /* Copy of kernel globals which is suitable for concurrent access from multiple threads.
   *
   * More specifically, the `kernel_globals_` is local to each threads and nobody else is
   * accessing it, but some "localization" is required to decouple from kernel globals stored
   * on the device level. */
  vector<CPUKernelThreadGlobals> kernel_thread_globals_;
};

CCL_NAMESPACE_END
