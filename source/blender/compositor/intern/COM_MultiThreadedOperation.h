/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 Blender Foundation. */

#pragma once

#include "COM_NodeOperation.h"

namespace blender::compositor {

class MultiThreadedOperation : public NodeOperation {
 protected:
  /**
   * Number of execution passes.
   */
  int num_passes_;
  /**
   * Current execution pass.
   */
  int current_pass_;

 protected:
  MultiThreadedOperation();

  /**
   * Called before an update memory buffer pass is executed. Single-threaded calls.
   */
  virtual void update_memory_buffer_started(MemoryBuffer *UNUSED(output),
                                            const rcti &UNUSED(area),
                                            Span<MemoryBuffer *> UNUSED(inputs))
  {
  }

  /**
   * Executes operation updating a memory buffer area. Multi-threaded calls.
   */
  virtual void update_memory_buffer_partial(MemoryBuffer *output,
                                            const rcti &area,
                                            Span<MemoryBuffer *> inputs) = 0;

  /**
   * Called after an update memory buffer pass is executed. Single-threaded calls.
   */
  virtual void update_memory_buffer_finished(MemoryBuffer *UNUSED(output),
                                             const rcti &UNUSED(area),
                                             Span<MemoryBuffer *> UNUSED(inputs))
  {
  }

 private:
  void update_memory_buffer(MemoryBuffer *output,
                            const rcti &area,
                            Span<MemoryBuffer *> inputs) override;
};

}  // namespace blender::compositor
