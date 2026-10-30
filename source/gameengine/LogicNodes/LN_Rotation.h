/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file LN_Rotation.h
 *  \ingroup logicnodes
 *
 * Logic Nodes' game-runtime rotation value. Blender-facing node sockets remain
 * Euler/radian values; conversion happens once at the game-engine boundary.
 */

#pragma once

#include <algorithm>
#include <cmath>

#include "MT_Matrix3x3.h"
#include "MT_Quaternion.h"
#include "MT_Vector3.h"

class alignas(16) LN_Rotation {
 public:
  LN_Rotation() : m_quaternion(0.0f, 0.0f, 0.0f, 1.0f)
  {
  }

  explicit LN_Rotation(const MT_Quaternion &quaternion)
      : m_quaternion(NormalizedOrIdentity(quaternion))
  {
  }

  static LN_Rotation Identity()
  {
    return LN_Rotation();
  }

  static LN_Rotation FromEulerXYZ(const MT_Vector3 &euler_radians)
  {
    if (!VectorIsFinite(euler_radians)) {
      return Identity();
    }
    return LN_Rotation(MT_Matrix3x3(euler_radians).getRotation());
  }

  static LN_Rotation FromMatrix(const MT_Matrix3x3 &matrix)
  {
    if (!MatrixIsFinite(matrix)) {
      return Identity();
    }

    /* Pure orientation matrices are overwhelmingly the common runtime case. Avoid the polar
     * decomposition below when their columns are already a proper orthonormal basis. */
    const MT_Vector3 source_x = matrix.getColumn(0);
    const MT_Vector3 source_y = matrix.getColumn(1);
    const MT_Vector3 source_z = matrix.getColumn(2);
    const double source_x_length_squared = double(source_x.length2());
    const double source_y_length_squared = double(source_y.length2());
    const double source_z_length_squared = double(source_z.length2());
    const double source_determinant = double(matrix.determinant());
    constexpr double orthonormal_tolerance = 1.0e-5;
    if (source_determinant > 0.0 &&
        std::fabs(source_determinant - 1.0) <= orthonormal_tolerance &&
        std::fabs(source_x_length_squared - 1.0) <= orthonormal_tolerance &&
        std::fabs(source_y_length_squared - 1.0) <= orthonormal_tolerance &&
        std::fabs(source_z_length_squared - 1.0) <= orthonormal_tolerance &&
        std::fabs(double(source_x.dot(source_y))) <= orthonormal_tolerance &&
        std::fabs(double(source_x.dot(source_z))) <= orthonormal_tolerance &&
        std::fabs(double(source_y.dot(source_z))) <= orthonormal_tolerance)
    {
      return LN_Rotation(matrix.getRotation());
    }

    /* Rotation inputs may cross a matrix boundary with scale or shear attached. A Newton polar
     * decomposition extracts the closest orthogonal factor and is symmetric with respect to
     * row- and column-side contamination. Work in normalized double precision so extreme but
     * finite input magnitudes cannot overflow the iteration. */
    double max_element = 0.0;
    double polar[3][3];
    for (int row = 0; row < 3; row++) {
      for (int column = 0; column < 3; column++) {
        max_element = std::max(max_element, std::fabs(double(matrix[row][column])));
      }
    }
    if (!std::isfinite(max_element) || max_element <= 1.0e-20) {
      return Identity();
    }
    for (int row = 0; row < 3; row++) {
      for (int column = 0; column < 3; column++) {
        polar[row][column] = double(matrix[row][column]) / max_element;
      }
    }

    for (int iteration = 0; iteration < 10; iteration++) {
      const double a = polar[0][0];
      const double b = polar[0][1];
      const double c = polar[0][2];
      const double d = polar[1][0];
      const double e = polar[1][1];
      const double f = polar[1][2];
      const double g = polar[2][0];
      const double h = polar[2][1];
      const double i = polar[2][2];
      const double determinant = a * (e * i - f * h) - b * (d * i - f * g) +
                                 c * (d * h - e * g);
      if (!std::isfinite(determinant) || std::fabs(determinant) <= 1.0e-14) {
        return Identity();
      }

      /* Cofactor(M) is inverse-transpose(M) * determinant(M). */
      const double inverse_transpose[3][3] = {
          {(e * i - f * h) / determinant,
           (f * g - d * i) / determinant,
           (d * h - e * g) / determinant},
          {(c * h - b * i) / determinant,
           (a * i - c * g) / determinant,
           (b * g - a * h) / determinant},
          {(b * f - c * e) / determinant,
           (c * d - a * f) / determinant,
           (a * e - b * d) / determinant},
      };

      double next[3][3];
      double max_delta = 0.0;
      double max_next = 0.0;
      for (int row = 0; row < 3; row++) {
        for (int column = 0; column < 3; column++) {
          next[row][column] = 0.5 * (polar[row][column] +
                                      inverse_transpose[row][column]);
          if (!std::isfinite(next[row][column])) {
            return Identity();
          }
          max_delta = std::max(max_delta,
                               std::fabs(next[row][column] - polar[row][column]));
          max_next = std::max(max_next, std::fabs(next[row][column]));
        }
      }
      for (int row = 0; row < 3; row++) {
        for (int column = 0; column < 3; column++) {
          polar[row][column] = next[row][column];
        }
      }
      if (max_delta <= 1.0e-7 * std::max(1.0, max_next)) {
        break;
      }
    }

    const double polar_determinant =
        polar[0][0] * (polar[1][1] * polar[2][2] - polar[1][2] * polar[2][1]) -
        polar[0][1] * (polar[1][0] * polar[2][2] - polar[1][2] * polar[2][0]) +
        polar[0][2] * (polar[1][0] * polar[2][1] - polar[1][1] * polar[2][0]);
    if (!std::isfinite(polar_determinant) || std::fabs(polar_determinant) <= 1.0e-8) {
      return Identity();
    }
    if (polar_determinant < 0.0) {
      /* A reflection is not a rotation. Select the least energetic source axis to remove so the
       * result is deterministic and perturbs the original matrix as little as possible. */
      int reflection_axis = 0;
      double shortest_axis = source_x_length_squared;
      if (source_y_length_squared < shortest_axis) {
        reflection_axis = 1;
        shortest_axis = source_y_length_squared;
      }
      if (source_z_length_squared < shortest_axis) {
        reflection_axis = 2;
      }
      for (int row = 0; row < 3; row++) {
        polar[row][reflection_axis] = -polar[row][reflection_axis];
      }
    }

    MT_Vector3 x{float(polar[0][0]), float(polar[1][0]), float(polar[2][0])};
    MT_Vector3 y{float(polar[0][1]), float(polar[1][1]), float(polar[2][1])};
    const float x_length_squared = float(x.length2());
    if (!std::isfinite(x_length_squared) || x_length_squared <= 1.0e-20f) {
      return Identity();
    }
    x /= std::sqrt(x_length_squared);
    y -= x * x.dot(y);
    const float y_length_squared = float(y.length2());
    if (!std::isfinite(y_length_squared) || y_length_squared <= 1.0e-20f) {
      return Identity();
    }
    y /= std::sqrt(y_length_squared);
    const MT_Vector3 z = x.cross(y);
    const MT_Matrix3x3 orthonormal(x.x(),
                                   y.x(),
                                   z.x(),
                                   x.y(),
                                   y.y(),
                                   z.y(),
                                   x.z(),
                                   y.z(),
                                   z.z());
    return LN_Rotation(orthonormal.getRotation());
  }

  const MT_Quaternion &Quaternion() const
  {
    return m_quaternion;
  }

  MT_Matrix3x3 ToMatrix() const
  {
    return MT_Matrix3x3(m_quaternion);
  }

  MT_Vector3 ToEulerXYZ() const
  {
    MT_Scalar x = 0.0f;
    MT_Scalar y = 0.0f;
    MT_Scalar z = 0.0f;
    ToMatrix().getEuler(x, y, z);
    return MT_Vector3(x, y, z);
  }

  LN_Rotation Slerp(const LN_Rotation &other, float factor) const
  {
    if (!std::isfinite(factor)) {
      factor = 0.0f;
    }
    if (factor == 0.0f) {
      return *this;
    }
    if (factor == 1.0f) {
      return other;
    }
    return LN_Rotation(m_quaternion.slerp(other.m_quaternion, MT_Scalar(factor)));
  }

  float AngleTo(const LN_Rotation &other) const
  {
    /* acos(dot(q1, q2)) loses every useful bit when two float quaternions are separated by less
     * than roughly 0.04 degrees: their dot product rounds to exactly one. The vector part of the
     * relative quaternion still carries that small angle accurately. atan2 also remains stable at
     * half turns, and abs(w) handles the q/-q double cover without a branch. */
    const MT_Quaternion relative = m_quaternion.conjugate() * other.m_quaternion;
    const double sine_half = std::hypot(
        double(relative.x()), double(relative.y()), double(relative.z()));
    const double cosine_half = std::fabs(double(relative.w()));
    return float(2.0 * std::atan2(sine_half, cosine_half));
  }

  bool NearlyEquals(const LN_Rotation &other, const float angular_tolerance) const
  {
    return AngleTo(other) <= std::max(0.0f, angular_tolerance);
  }

  bool IsIdentity(const float angular_tolerance = 1.0e-6f) const
  {
    return AngleTo(Identity()) <= std::max(0.0f, angular_tolerance);
  }

  /**
   * Return a deterministic representative for serialization and hashing.
   * q and -q encode the same orientation; choosing w >= 0 prevents needless
   * save-state and cache churn.
   */
  LN_Rotation Canonicalized() const
  {
    const float w = float(m_quaternion.w());
    const bool negative = w < 0.0f ||
                          (w == 0.0f &&
                           (m_quaternion.x() < 0.0f ||
                            (m_quaternion.x() == 0.0f &&
                             (m_quaternion.y() < 0.0f ||
                              (m_quaternion.y() == 0.0f && m_quaternion.z() < 0.0f)))));
    if (!negative) {
      return *this;
    }
    return LN_Rotation(MT_Quaternion(-m_quaternion.x(),
                                     -m_quaternion.y(),
                                     -m_quaternion.z(),
                                     -m_quaternion.w()));
  }

 private:
  static bool VectorIsFinite(const MT_Vector3 &vector)
  {
    return std::isfinite(float(vector.x())) && std::isfinite(float(vector.y())) &&
           std::isfinite(float(vector.z()));
  }

  static bool MatrixIsFinite(const MT_Matrix3x3 &matrix)
  {
    for (int row = 0; row < 3; row++) {
      for (int column = 0; column < 3; column++) {
        if (!std::isfinite(float(matrix[row][column]))) {
          return false;
        }
      }
    }
    return true;
  }

  static MT_Quaternion NormalizedOrIdentity(const MT_Quaternion &quaternion)
  {
    if (!std::isfinite(float(quaternion.x())) || !std::isfinite(float(quaternion.y())) ||
        !std::isfinite(float(quaternion.z())) || !std::isfinite(float(quaternion.w())))
    {
      return MT_Quaternion(0.0f, 0.0f, 0.0f, 1.0f);
    }
    const float length_squared = float(quaternion.length2());
    if (!std::isfinite(length_squared) || length_squared <= 1.0e-20f) {
      return MT_Quaternion(0.0f, 0.0f, 0.0f, 1.0f);
    }
    const float inverse_length = 1.0f / std::sqrt(length_squared);
    return MT_Quaternion(quaternion.x() * inverse_length,
                         quaternion.y() * inverse_length,
                         quaternion.z() * inverse_length,
                         quaternion.w() * inverse_length);
  }

  MT_Quaternion m_quaternion;
};
