// Ceres Solver - A fast non-linear least squares minimizer
// Copyright 2018 Google Inc. All rights reserved.
// http://ceres-solver.org/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// * Neither the name of Google Inc. nor the names of its contributors may be
//   used to endorse or promote products derived from this software without
//   specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Author: alexs.mac@gmail.com (Alex Stewart)

// This include must come before any #ifndef check on Ceres compile options.
#include "ceres/internal/config.h"

#ifdef CERES_NO_THREADS

#include "ceres/parallel_for.h"
#include "glog/logging.h"

namespace ceres {
namespace internal {

int MaxNumThreadsAvailable() { return 1; }

void ParallelFor(ContextImpl* context,
                 int start,
                 int end,
                 int num_threads,
                 const std::function<void(int)>& function) {
  CHECK_GT(num_threads, 0);
  CHECK(context != nullptr);
  if (end <= start) {
    return;
  }
  for (int i = start; i < end; ++i) {
    function(i);
  }
}

void ParallelFor(ContextImpl* context,
                 int start,
                 int end,
                 int num_threads,
                 const std::function<void(int thread_id, int i)>& function) {
  CHECK_GT(num_threads, 0);
  CHECK(context != nullptr);
  if (end <= start) {
    return;
  }
  const int thread_id = 0;
  for (int i = start; i < end; ++i) {
    function(thread_id, i);
  }
}

}  // namespace internal
}  // namespace ceres

#endif  // CERES_NO_THREADS
