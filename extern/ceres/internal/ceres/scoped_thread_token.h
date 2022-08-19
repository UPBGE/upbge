// Ceres Solver - A fast non-linear least squares minimizer
// Copyright 2017 Google Inc. All rights reserved.
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
// Author: yp@photonscore.de (Yury Prokazov)

#ifndef CERES_INTERNAL_SCOPED_THREAD_TOKEN_H_
#define CERES_INTERNAL_SCOPED_THREAD_TOKEN_H_

#include "ceres/internal/export.h"
#include "ceres/thread_token_provider.h"

namespace ceres {
namespace internal {

// Helper class for ThreadTokenProvider. This object acquires a token in its
// constructor and puts that token back with destruction.
class CERES_NO_EXPORT ScopedThreadToken {
 public:
  explicit ScopedThreadToken(ThreadTokenProvider* provider)
      : provider_(provider), token_(provider->Acquire()) {}

  ~ScopedThreadToken() { provider_->Release(token_); }
  ScopedThreadToken(ScopedThreadToken&) = delete;
  ScopedThreadToken& operator=(ScopedThreadToken&) = delete;

  int token() const { return token_; }

 private:
  ThreadTokenProvider* provider_;
  int token_;
};

}  // namespace internal
}  // namespace ceres

#endif  // CERES_INTERNAL_SCOPED_THREAD_TOKEN_H_
