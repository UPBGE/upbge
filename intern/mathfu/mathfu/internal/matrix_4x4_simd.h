/*
* Copyright 2014 Google Inc. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#ifndef MATHFU_MATRIX_4X4_SIMD_H_
#define MATHFU_MATRIX_4X4_SIMD_H_

#include "mathfu/matrix.h"

#ifdef MATHFU_COMPILE_WITH_SIMD
#include "vectorial/simd4x4f.h"
#endif

/// @file mathfu/internal/matrix_4x4_simd.h MathFu Matrix<T, 4, 4>
///       Specialization
/// @brief 4x4 specialization of mathfu::Matrix for SIMD optimized builds.
/// @see mathfu::Matrix

namespace mathfu {

#ifdef MATHFU_COMPILE_WITH_SIMD

static const Vector<float, 4> kAffineWColumn(0.0f, 0.0f, 0.0f, 1.0f);

/// @cond MATHFU_INTERNAL
template <>
class Matrix<float, 4> {
 public:
  Matrix<float, 4>() {}

  inline Matrix<float, 4>(const Matrix<float, 4>& m) {
    simd.x = m.simd.x;
    simd.y = m.simd.y;
    simd.z = m.simd.z;
    simd.w = m.simd.w;
  }

  explicit inline Matrix<float, 4>(const float& s) {
    simd4f v = simd4f_create(s, s, s, s);
    simd = simd4x4f_create(v, v, v, v);
  }

  inline Matrix<float, 4>(const float& s00, const float& s10, const float& s20,
                          const float& s30, const float& s01, const float& s11,
                          const float& s21, const float& s31, const float& s02,
                          const float& s12, const float& s22, const float& s32,
                          const float& s03, const float& s13, const float& s23,
                          const float& s33) {
    simd = simd4x4f_create(
        simd4f_create(s00, s10, s20, s30), simd4f_create(s01, s11, s21, s31),
        simd4f_create(s02, s12, s22, s32), simd4f_create(s03, s13, s23, s33));
  }

  explicit inline Matrix<float, 4>(const float* m) {
    simd =
        simd4x4f_create(simd4f_create(m[0], m[1], m[2], m[3]),
                        simd4f_create(m[4], m[5], m[6], m[7]),
                        simd4f_create(m[8], m[9], m[10], m[11]),
                        simd4f_create(m[12], m[13], m[14], m[15]));
  }

  explicit inline Matrix<float, 4>(const float m[4][4]) {
    simd =
        simd4x4f_create(simd4f_create(m[0][0], m[0][1], m[0][2], m[0][3]),
                        simd4f_create(m[1][0], m[1][1], m[1][2], m[1][3]),
                        simd4f_create(m[2][0], m[2][1], m[2][2], m[2][3]),
                        simd4f_create(m[3][0], m[3][1], m[3][2], m[3][3]));
  }

  inline Matrix<float, 4>(const Vector<float, 4>& column0,
                          const Vector<float, 4>& column1,
                          const Vector<float, 4>& column2,
                          const Vector<float, 4>& column3) {
#if defined(MATHFU_COMPILE_WITH_PADDING)
    simd = simd4x4f_create(column0.simd, column1.simd,
                                        column2.simd, column3.simd);
#else
    simd = simd4x4f_create(
        simd4f_create(column0[0], column0[1], column0[2], column0[3]),
        simd4f_create(column1[0], column1[1], column1[2], column1[3]),
        simd4f_create(column2[0], column2[1], column2[2], column2[3]),
        simd4f_create(column3[0], column3[1], column3[2], column3[3]));
#endif  // defined(MATHFU_COMPILE_WITH_PADDING)
  }

  explicit inline Matrix(const VectorPacked<float, 4>* const vectors) {
    simd.x = simd4f_uload4(vectors[0].data);
    simd.y = simd4f_uload4(vectors[1].data);
    simd.z = simd4f_uload4(vectors[2].data);
    simd.w = simd4f_uload4(vectors[3].data);
  }

  inline const float& operator()(const int i, const int j) const {
	return data_[j][i];
  }

  inline float& operator()(const int i, const int j) {
	return data_[j][i];
  }

  inline const float& operator()(const int i) const {
    return this->operator[](i);
  }

  inline float& operator()(const int i) { return this->operator[](i); }

  inline const float& operator[](const int i) const {
    return reinterpret_cast<const float*>(data_)[i];
  }

  inline float& operator[](const int i) {
    return reinterpret_cast<float*>(data_)[i];
  }

  inline void Pack(VectorPacked<float, 4>* const vector) const {
    simd4f_ustore4(simd.x, vector[0].data);
    simd4f_ustore4(simd.y, vector[1].data);
    simd4f_ustore4(simd.z, vector[2].data);
    simd4f_ustore4(simd.w, vector[3].data);
  }

  inline void Pack(float a[4][4]) const {
    MATHFU_MAT_OPERATION(simd4f_ustore4(data_[i].simd, a[i]));
  }

  inline void Pack(float a[16]) const {
    MATHFU_MAT_OPERATION(simd4f_ustore4(data_[i].simd, &a[i * 4]));
  }

  inline float (&Data() const)[4][4] WARN_UNUSED_RESULT {
	  return const_cast<float (&)[4][4]>(float_data_);
  }

  inline Vector<float, 4>& GetColumn(const int i) WARN_UNUSED_RESULT {
    return data_[i];
  }

  inline const Vector<float, 4>& GetColumn(const int i) const WARN_UNUSED_RESULT {
    return data_[i];
  }

  inline Vector<float, 4> GetRow(const int i) const WARN_UNUSED_RESULT {
    Vector<float, 4> v;
    for (int j = 0; j < 4; ++j) {
      v[j] = data_[j][i];
    }
    return v;
  }

  inline Matrix<float, 4> operator-() const {
    Matrix<float, 4> m(0.f);
    simd4x4f_sub(&m.simd, &simd,
                 &m.simd);
    return m;
  }

  inline Matrix<float, 4> operator+(const Matrix<float, 4>& m) const {
    Matrix<float, 4> return_m;
    simd4x4f_add(&simd, &m.simd,
                 &return_m.simd);
    return return_m;
  }

  inline Matrix<float, 4> operator-(const Matrix<float, 4>& m) const {
    Matrix<float, 4> return_m;
    simd4x4f_sub(&simd, &m.simd,
                 &return_m.simd);
    return return_m;
  }

  inline Matrix<float, 4> operator*(const float& s) const {
    Matrix<float, 4> m(s);
    simd4x4f_mul(&m.simd, &simd,
                 &m.simd);
    return m;
  }

  inline Matrix<float, 4> operator/(const float& s) const {
    Matrix<float, 4> m(1 / s);
    simd4x4f_mul(&m.simd, &simd,
                 &m.simd);
    return m;
  }

  inline Vector<float, 3> operator*(const Vector<float, 3>& v) const {
    Vector<float, 3> return_v;
    simd4f temp_simd = simd4f_create(v[0], v[1], v[2], 1.0f);
    simd4x4f_matrix_vector_mul(&simd, &temp_simd, &return_v.simd);
    return_v *= (1.0f / return_v.data_[3]);
    return return_v;
  }

  inline Vector<float, 4> operator*(const Vector<float, 4>& v) const {
    Vector<float, 4> return_v;
    simd4x4f_matrix_vector_mul(&simd, &v.simd,
                               &return_v.simd);
    return return_v;
  }

  inline Matrix<float, 4> operator*(const Matrix<float, 4>& m) const {
    Matrix<float, 4> return_m;
    simd4x4f_matrix_mul(&simd, &m.simd,
                        &return_m.simd);
    return return_m;
  }

  friend inline Vector<float, 4> operator*(const Vector<float, 4>& v, const Matrix<float, 4>& m) {
    return Vector<float, 4>(
      simd4f_get_x(simd4f_dot4(v.simd, m.simd.x)),
      simd4f_get_x(simd4f_dot4(v.simd, m.simd.y)),
      simd4f_get_x(simd4f_dot4(v.simd, m.simd.z)),
      simd4f_get_x(simd4f_dot4(v.simd, m.simd.w)));
  }

  friend inline Vector<float, 3> operator*(const Vector<float, 3>& v, const Matrix<float, 4>& m) {
    Vector<float, 3> return_v;
    simd4f temp_simd = simd4f_create(v[0], v[1], v[2], 1.0f);

    return_v.data_[0] = simd4f_get_x(simd4f_dot4(temp_simd, m.simd.x));
    return_v.data_[1] = simd4f_get_x(simd4f_dot4(temp_simd, m.simd.y));
    return_v.data_[2] = simd4f_get_x(simd4f_dot4(temp_simd, m.simd.z));
    return_v.data_[3] = simd4f_get_x(simd4f_dot4(temp_simd, m.simd.w));

    return_v *= (1.0f / return_v.data_[3]);

    return return_v;
  }

  inline Matrix<float, 4> Inverse() const WARN_UNUSED_RESULT {
    Matrix<float, 4> return_m;
    simd4x4f_inverse(&simd, &return_m.simd);
    return return_m;
  }

  inline bool InverseWithDeterminantCheck(
      Matrix<float, 4, 4>* const inverse) const {
    return fabs(simd4f_get_x(simd4x4f_inverse(&simd,
                                              &inverse->simd))) >=
           Constants<float>::GetDeterminantThreshold();
  }

  /// Calculate the transpose of matrix.
  /// @return The transpose of the specified matrix.
  inline Matrix<float, 4, 4> Transpose() const WARN_UNUSED_RESULT {
    Matrix<float, 4, 4> transpose;
    simd4x4f_transpose(&simd, &transpose.simd);
    return transpose;
  }

  inline Vector<float, 3> TranslationVector3D() const WARN_UNUSED_RESULT {
    return Vector<float, 3>(simd.w);
  }

  inline Matrix<float, 3> RotationMatrix() const WARN_UNUSED_RESULT {
    return ToRotationMatrix(*this);
  }

  inline Vector<float, 3> ScaleVector3D() const WARN_UNUSED_RESULT {
    return ToScaleVector3DHelper(*this);
  }

  inline Matrix<float, 4>& operator+=(const Matrix<float, 4>& m) {
    simd4x4f_add(&simd, &m.simd, &simd);
    return *this;
  }

  inline Matrix<float, 4>& operator-=(const Matrix<float, 4>& m) {
    simd4x4f_sub(&simd, &m.simd, &simd);
    return *this;
  }

  inline Matrix<float, 4>& operator*=(const float& s) {
    Matrix<float, 4> m(s);
    simd4x4f_mul(&m.simd, &simd, &simd);
    return *this;
  }

  inline Matrix<float, 4>& operator/=(const float& s) {
    Matrix<float, 4> m(1 / s);
    simd4x4f_mul(&m.simd, &simd, &simd);
    return *this;
  }

  inline Matrix<float, 4> operator*=(const Matrix<float, 4>& m) {
    Matrix<float, 4> copy_of_this(*this);
    simd4x4f_matrix_mul(&copy_of_this.simd, &m.simd,
                        &simd);
    return *this;
  }

  template <typename CompatibleT>
  static inline WARN_UNUSED_RESULT Matrix<float, 4> FromType(const CompatibleT& compatible) {
    return FromTypeHelper<float, 4, 4, CompatibleT>(compatible);
  }

  template <typename CompatibleT>
  static inline WARN_UNUSED_RESULT CompatibleT ToType(const Matrix<float, 4>& m) {
    return ToTypeHelper<float, 4, 4, CompatibleT>(m);
  }

  static inline WARN_UNUSED_RESULT Matrix<float, 4> OuterProduct(const Vector<float, 4>& v1,
                                              const Vector<float, 4>& v2) {
    Matrix<float, 4> m;
    m.simd =
        simd4x4f_create(simd4f_mul(v1.simd, simd4f_splat(v2[0])),
                        simd4f_mul(v1.simd, simd4f_splat(v2[1])),
                        simd4f_mul(v1.simd, simd4f_splat(v2[2])),
                        simd4f_mul(v1.simd, simd4f_splat(v2[3])));
    return m;
  }

  static inline WARN_UNUSED_RESULT Matrix<float, 4> HadamardProduct(const Matrix<float, 4>& m1,
                                                 const Matrix<float, 4>& m2) WARN_UNUSED_RESULT {
    Matrix<float, 4> return_m;
    simd4x4f_mul(&m1.simd, &m2.simd,
                 &return_m.simd);
    return return_m;
  }

  static inline WARN_UNUSED_RESULT Matrix<float, 4> Identity() WARN_UNUSED_RESULT {
    Matrix<float, 4> return_m;
    simd4x4f_identity(&return_m.simd);
    return return_m;
  }

  static inline WARN_UNUSED_RESULT Matrix<float, 4> FromTranslationVector(
      const Vector<float, 3>& v) WARN_UNUSED_RESULT {
    return Matrix<float, 4>(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, v[0], v[1],
                            v[2], 1);
  }

  static inline WARN_UNUSED_RESULT Vector<float, 4> ToScaleVector(const Matrix<float, 4, 4>& m) WARN_UNUSED_RESULT {
    return ToScaleVectorHelper(m);
  }

  static inline WARN_UNUSED_RESULT Matrix<float, 4> FromScaleVector(const Vector<float, 3>& v) WARN_UNUSED_RESULT {
    return Matrix<float, 4>(v[0], 0, 0, 0, 0, v[1], 0, 0, 0, 0, v[2], 0, 0, 0,
                            0, 1);
  }

  static inline WARN_UNUSED_RESULT Matrix<float, 3> ToRotationMatrix(const Matrix<float, 4>& m) WARN_UNUSED_RESULT {
    return Matrix<float, 3>(m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9],
                        m[10]);
  }

  static inline WARN_UNUSED_RESULT Matrix<float, 4> FromRotationMatrix(const Matrix<float, 3>& m) WARN_UNUSED_RESULT {
    return Matrix<float, 4>(m[0], m[1], m[2], 0, m[3], m[4], m[5], 0, m[6],
                            m[7], m[8], 0, 0, 0, 0, 1);
  }

  /// @brief Constructs a Matrix<float, 4> from an AffineTransform.
  ///
  /// @param affine An AffineTransform reference to be used to construct
  /// a Matrix<float, 4> by adding in the 'w' row of [0, 0, 0, 1].
  static inline WARN_UNUSED_RESULT Matrix<float, 4> FromAffineTransform(
      const AffineTransform& affine) WARN_UNUSED_RESULT {
    Matrix<float, 4> m;
    m.simd.x = simd4f_create(affine[0], affine[1], affine[2], 0.0f);
    m.simd.y = simd4f_create(affine[3], affine[4], affine[5], 0.0f);
    m.simd.z = simd4f_create(affine[6], affine[7], affine[8], 0.0f);
    m.simd.w = simd4f_create(affine[9], affine[10], affine[11], 1.0f);
    return m;
  }

  /// @brief Converts a Matrix<float, 4> into an AffineTransform.
  ///
  /// @param m A Matrix<float, 4> reference to be converted into an
  /// AffineTransform by dropping the fixed 'w' row.
  ///
  /// @return Returns an AffineTransform that contains the essential
  /// transformation data from the Matrix<float, 4>.
  static inline WARN_UNUSED_RESULT AffineTransform ToAffineTransform(const Matrix<float, 4>& m) WARN_UNUSED_RESULT {
    return AffineTransform(m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9],
                           m[10], m[12], m[13], m[14]);
  }

  /// Create a 4x4 perpective matrix.
  /// @handedness: 1.0f for RH, -1.0f for LH
  static inline WARN_UNUSED_RESULT Matrix<float, 4, 4> Perspective(float fovy, float aspect,
                                                float znear, float zfar,
                                                float handedness = 1.0f) WARN_UNUSED_RESULT {
    return PerspectiveHelper(fovy, aspect, znear, zfar, handedness);
  }

  /// Create a 4x4 perpective matrix.
  /// @handedness: 1.0f for RH, -1.0f for LH
  static inline WARN_UNUSED_RESULT Matrix<float, 4, 4> Perspective(float left, float right,
												float bottom, float top,
                                                float znear, float zfar,
                                                float handedness = 1.0f) WARN_UNUSED_RESULT {
    return PerspectiveHelper(left, right, bottom, top, znear, zfar, handedness);
  }

  /// Create a 4x4 orthographic matrix.
  /// @param handedness 1.0f for RH, -1.0f for LH
  static inline WARN_UNUSED_RESULT Matrix<float, 4, 4> Ortho(float left, float right, float bottom,
                                          float top, float znear, float zfar,
                                          float handedness = 1.0f) WARN_UNUSED_RESULT {
    return OrthoHelper(left, right, bottom, top, znear, zfar, handedness);
  }

  /// Create a 3-dimensional camera matrix.
  /// @param at The look-at target of the camera.
  /// @param eye The position of the camera.
  /// @param up The up vector in the world, for example (0, 1, 0) if the
  /// @handedness: 1.0f for RH, -1.0f for LH
  /// TODO: Change default handedness to 1.0f, to match Perspective().
  /// y-axis is up.
  static inline WARN_UNUSED_RESULT Matrix<float, 4, 4> LookAt(const Vector<float, 3>& at,
                                           const Vector<float, 3>& eye,
                                           const Vector<float, 3>& up,
                                           float handedness = -1.0f) WARN_UNUSED_RESULT {
    return LookAtHelper(at, eye, up, handedness);
  }

  /// @brief Get the 3D position in object space from a window coordinate.
  ///
  /// @param window_coord The window coordinate. The z value is for depth.
  /// A window coordinate on the near plane will have 0 as the z value.
  /// And a window coordinate on the far plane will have 1 as the z value.
  /// z value should be with in [0, 1] here.
  /// @param model_view The Model View matrix.
  /// @param projection The projection matrix.
  /// @param window_width Width of the window.
  /// @param window_height Height of the window.
  /// @return the mapped 3D position in object space.
  static inline WARN_UNUSED_RESULT Vector<float, 3> UnProject(
      const Vector<float, 3>& window_coord,
      const Matrix<float, 4, 4>& model_view,
      const Matrix<float, 4, 4>& projection, const float window_width,
      const float window_height) WARN_UNUSED_RESULT {
    Vector<float, 3> result;
    UnProjectHelper(window_coord, model_view, projection, window_width,
                    window_height, result);
    return result;
  }

  // Dimensions of the matrix.
  /// Number of rows in the matrix.
  static const int kRows = 4;
  /// Number of columns in the matrix.
  static const int kColumns = 4;
  /// Total number of elements in the matrix.
  static const int kElements = 4 * 4;

  MATHFU_DEFINE_CLASS_SIMD_AWARE_NEW_DELETE

  // Contents of the Matrix in different representations to work around
  // strict aliasing rules.
  union {
    simd4x4f simd;
    struct {
      Vector<float, 4> data_[4];
    };
	float float_data_[4][4];
  };
};

inline Matrix<float, 4> operator*(const float& s, const Matrix<float, 4>& m) {
  return m * s;
}

/// @endcond
#endif  // MATHFU_COMPILE_WITH_SIMD
}  // namespace mathfu

#endif  // MATHFU_MATRIX_4X4_SIMD_H_
