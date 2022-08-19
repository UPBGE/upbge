// Ceres Solver - A fast non-linear least squares minimizer
// Copyright 2022 Google Inc. All rights reserved.
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
// Author: sameeragarwal@google.com (Sameer Agarwal)

#include "ceres/local_parameterization.h"

#include <algorithm>

#include "Eigen/Geometry"
#include "ceres/internal/eigen.h"
#include "ceres/internal/fixed_array.h"
#include "ceres/internal/householder_vector.h"
#include "ceres/rotation.h"
#include "glog/logging.h"

namespace ceres {

using std::vector;

LocalParameterization::~LocalParameterization() = default;

bool LocalParameterization::MultiplyByJacobian(const double* x,
                                               const int num_rows,
                                               const double* global_matrix,
                                               double* local_matrix) const {
  if (LocalSize() == 0) {
    return true;
  }

  Matrix jacobian(GlobalSize(), LocalSize());
  if (!ComputeJacobian(x, jacobian.data())) {
    return false;
  }

  MatrixRef(local_matrix, num_rows, LocalSize()) =
      ConstMatrixRef(global_matrix, num_rows, GlobalSize()) * jacobian;
  return true;
}

IdentityParameterization::IdentityParameterization(const int size)
    : size_(size) {
  CHECK_GT(size, 0);
}

bool IdentityParameterization::Plus(const double* x,
                                    const double* delta,
                                    double* x_plus_delta) const {
  VectorRef(x_plus_delta, size_) =
      ConstVectorRef(x, size_) + ConstVectorRef(delta, size_);
  return true;
}

bool IdentityParameterization::ComputeJacobian(const double* x,
                                               double* jacobian) const {
  MatrixRef(jacobian, size_, size_).setIdentity();
  return true;
}

bool IdentityParameterization::MultiplyByJacobian(const double* x,
                                                  const int num_cols,
                                                  const double* global_matrix,
                                                  double* local_matrix) const {
  std::copy(
      global_matrix, global_matrix + num_cols * GlobalSize(), local_matrix);
  return true;
}

SubsetParameterization::SubsetParameterization(
    int size, const vector<int>& constant_parameters)
    : local_size_(size - constant_parameters.size()), constancy_mask_(size, 0) {
  if (constant_parameters.empty()) {
    return;
  }

  vector<int> constant = constant_parameters;
  std::sort(constant.begin(), constant.end());
  CHECK_GE(constant.front(), 0) << "Indices indicating constant parameter must "
                                   "be greater than equal to zero.";
  CHECK_LT(constant.back(), size)
      << "Indices indicating constant parameter must be less than the size "
      << "of the parameter block.";
  CHECK(std::adjacent_find(constant.begin(), constant.end()) == constant.end())
      << "The set of constant parameters cannot contain duplicates";
  for (int parameter : constant_parameters) {
    constancy_mask_[parameter] = 1;
  }
}

bool SubsetParameterization::Plus(const double* x,
                                  const double* delta,
                                  double* x_plus_delta) const {
  const int global_size = GlobalSize();
  for (int i = 0, j = 0; i < global_size; ++i) {
    if (constancy_mask_[i]) {
      x_plus_delta[i] = x[i];
    } else {
      x_plus_delta[i] = x[i] + delta[j++];
    }
  }
  return true;
}

bool SubsetParameterization::ComputeJacobian(const double* x,
                                             double* jacobian) const {
  if (local_size_ == 0) {
    return true;
  }

  const int global_size = GlobalSize();
  MatrixRef m(jacobian, global_size, local_size_);
  m.setZero();
  for (int i = 0, j = 0; i < global_size; ++i) {
    if (!constancy_mask_[i]) {
      m(i, j++) = 1.0;
    }
  }
  return true;
}

bool SubsetParameterization::MultiplyByJacobian(const double* x,
                                                const int num_cols,
                                                const double* global_matrix,
                                                double* local_matrix) const {
  if (local_size_ == 0) {
    return true;
  }

  const int global_size = GlobalSize();
  for (int col = 0; col < num_cols; ++col) {
    for (int i = 0, j = 0; i < global_size; ++i) {
      if (!constancy_mask_[i]) {
        local_matrix[col * local_size_ + j++] =
            global_matrix[col * global_size + i];
      }
    }
  }
  return true;
}

bool QuaternionParameterization::Plus(const double* x,
                                      const double* delta,
                                      double* x_plus_delta) const {
  const double norm_delta =
      sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
  if (norm_delta > 0.0) {
    const double sin_delta_by_delta = (sin(norm_delta) / norm_delta);
    double q_delta[4];
    q_delta[0] = cos(norm_delta);
    q_delta[1] = sin_delta_by_delta * delta[0];
    q_delta[2] = sin_delta_by_delta * delta[1];
    q_delta[3] = sin_delta_by_delta * delta[2];
    QuaternionProduct(q_delta, x, x_plus_delta);
  } else {
    for (int i = 0; i < 4; ++i) {
      x_plus_delta[i] = x[i];
    }
  }
  return true;
}

bool QuaternionParameterization::ComputeJacobian(const double* x,
                                                 double* jacobian) const {
  // clang-format off
  jacobian[0] = -x[1];  jacobian[1]  = -x[2];   jacobian[2]  = -x[3];
  jacobian[3] =  x[0];  jacobian[4]  =  x[3];   jacobian[5]  = -x[2];
  jacobian[6] = -x[3];  jacobian[7]  =  x[0];   jacobian[8]  =  x[1];
  jacobian[9] =  x[2];  jacobian[10] = -x[1];   jacobian[11] =  x[0];
  // clang-format on
  return true;
}

bool EigenQuaternionParameterization::Plus(const double* x_ptr,
                                           const double* delta,
                                           double* x_plus_delta_ptr) const {
  Eigen::Map<Eigen::Quaterniond> x_plus_delta(x_plus_delta_ptr);
  Eigen::Map<const Eigen::Quaterniond> x(x_ptr);

  const double norm_delta =
      sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
  if (norm_delta > 0.0) {
    const double sin_delta_by_delta = sin(norm_delta) / norm_delta;

    // Note, in the constructor w is first.
    Eigen::Quaterniond delta_q(cos(norm_delta),
                               sin_delta_by_delta * delta[0],
                               sin_delta_by_delta * delta[1],
                               sin_delta_by_delta * delta[2]);
    x_plus_delta = delta_q * x;
  } else {
    x_plus_delta = x;
  }

  return true;
}

bool EigenQuaternionParameterization::ComputeJacobian(const double* x,
                                                      double* jacobian) const {
  // clang-format off
  jacobian[0] =  x[3];  jacobian[1]  =  x[2];  jacobian[2]  = -x[1];
  jacobian[3] = -x[2];  jacobian[4]  =  x[3];  jacobian[5]  =  x[0];
  jacobian[6] =  x[1];  jacobian[7]  = -x[0];  jacobian[8]  =  x[3];
  jacobian[9] = -x[0];  jacobian[10] = -x[1];  jacobian[11] = -x[2];
  // clang-format on
  return true;
}

HomogeneousVectorParameterization::HomogeneousVectorParameterization(int size)
    : size_(size) {
  CHECK_GT(size_, 1) << "The size of the homogeneous vector needs to be "
                     << "greater than 1.";
}

bool HomogeneousVectorParameterization::Plus(const double* x_ptr,
                                             const double* delta_ptr,
                                             double* x_plus_delta_ptr) const {
  ConstVectorRef x(x_ptr, size_);
  ConstVectorRef delta(delta_ptr, size_ - 1);
  VectorRef x_plus_delta(x_plus_delta_ptr, size_);

  const double norm_delta = delta.norm();

  if (norm_delta == 0.0) {
    x_plus_delta = x;
    return true;
  }

  // Map the delta from the minimum representation to the over parameterized
  // homogeneous vector. See section A6.9.2 on page 624 of Hartley & Zisserman
  // (2nd Edition) for a detailed description.  Note there is a typo on Page
  // 625, line 4 so check the book errata.
  const double norm_delta_div_2 = 0.5 * norm_delta;
  const double sin_delta_by_delta =
      std::sin(norm_delta_div_2) / norm_delta_div_2;

  Vector y(size_);
  y.head(size_ - 1) = 0.5 * sin_delta_by_delta * delta;
  y(size_ - 1) = std::cos(norm_delta_div_2);

  Vector v(size_);
  double beta;

  // NOTE: The explicit template arguments are needed here because
  // ComputeHouseholderVector is templated and some versions of MSVC
  // have trouble deducing the type of v automatically.
  internal::ComputeHouseholderVector<ConstVectorRef, double, Eigen::Dynamic>(
      x, &v, &beta);

  // Apply the delta update to remain on the unit sphere. See section A6.9.3
  // on page 625 of Hartley & Zisserman (2nd Edition) for a detailed
  // description.
  x_plus_delta = x.norm() * (y - v * (beta * (v.transpose() * y)));

  return true;
}

bool HomogeneousVectorParameterization::ComputeJacobian(
    const double* x_ptr, double* jacobian_ptr) const {
  ConstVectorRef x(x_ptr, size_);
  MatrixRef jacobian(jacobian_ptr, size_, size_ - 1);

  Vector v(size_);
  double beta;

  // NOTE: The explicit template arguments are needed here because
  // ComputeHouseholderVector is templated and some versions of MSVC
  // have trouble deducing the type of v automatically.
  internal::ComputeHouseholderVector<ConstVectorRef, double, Eigen::Dynamic>(
      x, &v, &beta);

  // The Jacobian is equal to J = 0.5 * H.leftCols(size_ - 1) where H is the
  // Householder matrix (H = I - beta * v * v').
  for (int i = 0; i < size_ - 1; ++i) {
    jacobian.col(i) = -0.5 * beta * v(i) * v;
    jacobian.col(i)(i) += 0.5;
  }
  jacobian *= x.norm();

  return true;
}

bool ProductParameterization::Plus(const double* x,
                                   const double* delta,
                                   double* x_plus_delta) const {
  int x_cursor = 0;
  int delta_cursor = 0;
  for (const auto& param : local_params_) {
    if (!param->Plus(
            x + x_cursor, delta + delta_cursor, x_plus_delta + x_cursor)) {
      return false;
    }
    delta_cursor += param->LocalSize();
    x_cursor += param->GlobalSize();
  }

  return true;
}

bool ProductParameterization::ComputeJacobian(const double* x,
                                              double* jacobian_ptr) const {
  MatrixRef jacobian(jacobian_ptr, GlobalSize(), LocalSize());
  jacobian.setZero();
  internal::FixedArray<double> buffer(buffer_size_);

  int x_cursor = 0;
  int delta_cursor = 0;
  for (const auto& param : local_params_) {
    const int local_size = param->LocalSize();
    const int global_size = param->GlobalSize();

    if (!param->ComputeJacobian(x + x_cursor, buffer.data())) {
      return false;
    }
    jacobian.block(x_cursor, delta_cursor, global_size, local_size) =
        MatrixRef(buffer.data(), global_size, local_size);

    delta_cursor += local_size;
    x_cursor += global_size;
  }

  return true;
}

}  // namespace ceres
